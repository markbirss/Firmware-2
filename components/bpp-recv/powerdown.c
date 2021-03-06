#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "powerdown.h"

typedef struct PowerItem PowerItem;
static PowerDownCb *pdCb=NULL;
static void *pdCbArg=NULL;
static SemaphoreHandle_t mux;
static TimerHandle_t tmr;
static PowerMode powerMode;
static bool showDebugMsg;

RTC_DATA_ATTR uint64_t savedWakeTimestamp[NO_POWER_MODES];

#define ST_ACTIVE 0
#define ST_CANSLEEP 1
#define ST_CANSLEEP_UNTIL 2

struct PowerItem {
	int ref;
	int state;
	struct timeval sleepUntil; //WARNING: Also used to indicate when a powerHold expires.
#if POWERDOWN_DBG
	const char *fn;
	int line;
#endif
	PowerItem *next;
};

static uint64_t getCurrentTimeMs() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t ret=tv.tv_sec*1000+tv.tv_usec/1000;
	return ret;
}

static PowerItem *powerItems;

static PowerItem *findItem(int ref) {
	for (PowerItem *i=powerItems; i!=NULL; i=i->next) {
		if (i->ref==ref) return i;
	}
	//Not found. Add item.
	PowerItem *p=malloc(sizeof(PowerItem));
	memset(p, 0, sizeof(PowerItem));
	p->ref=ref;
	p->next=powerItems;
	powerItems=p;
	return p;
}


static void doSleep(int sleepMs, PowerMode nextMode) {
	if (pdCb) pdCb(sleepMs, pdCbArg, nextMode);
}

//Warning: very much not multithread-compatible. Probably isn't that big of a deal because
//of this being debugging code.
static const char *printref(const PowerItem *i) {
	static char buf[128];
#if POWERDOWN_DBG
	sprintf(buf, "%x (%s:%d)", i->ref, i->fn, i->line);
#else
	sprintf(buf, "%x", i->ref);
#endif
	return buf;
}

//Warning: non-multithread compatible, needs to be mutexed off.
static void checkCanSleep() {
	PowerItem *i=powerItems;
	int canSleepForMs=-1;
	int cannotSleep=0;
	while (i!=NULL) {
		//mssleep is the delay between sleepUntil and now.
		struct timeval now;
		gettimeofday(&now, NULL);
		int mssleep=(i->sleepUntil.tv_sec-now.tv_sec)*1000;
		mssleep+=((i->sleepUntil.tv_usec-now.tv_usec)/1000);
		if (i->state==ST_ACTIVE) {
			//WARNING: SleepUntil is abused as an indicator when the hold expires
			if (mssleep>0) {
				if (showDebugMsg) printf("Power: Ref %s: active (hold lasts %d more ms)\n", printref(i), mssleep);
				cannotSleep=1;
			} else {
				if (showDebugMsg) printf("Power: Ref %s: expired!\n", printref(i));
				i->state=ST_CANSLEEP;
			}
		} else if (i->state==ST_CANSLEEP_UNTIL) {
			if (mssleep<2000) {
				//Sleep req is too short.
				//We're going to ignore this sleep request, and make the thing active again.
				if (showDebugMsg) printf("Power: Ref %s: can sleep for %d ms. Too short, making active again.\n", printref(i), mssleep);
				i->state=ST_ACTIVE;
				cannotSleep=1;
			} else {
				if (showDebugMsg) printf("Power: Ref %s: can sleep for %d ms\n", printref(i), mssleep);
				if (canSleepForMs==-1 || mssleep<canSleepForMs) {
					canSleepForMs=mssleep;
				}
			}
		} else if (i->state==ST_CANSLEEP) {
//			printf("Power: Ref %x: can sleep.\n", i->ref);
			//Erm, nothing to check, thing can sleep.
		}
		i=i->next;
	}
	
	//See if one of the higher-priority power modes need to wake.
	for (int i=powerMode+1; i<NO_POWER_MODES; i++) {
		if (savedWakeTimestamp[i]!=0 && savedWakeTimestamp[i]<getCurrentTimeMs()) {
			//Yep. Immediately switch to this mode.
			doSleep(0, i);
			cannotSleep=1; //because we already called the callback
		}
	}
	
	//If we're here, we can sleep.
	if (!cannotSleep) {
		savedWakeTimestamp[powerMode]=getCurrentTimeMs()+canSleepForMs;
		//Figured out which mode needs to wake up next, by figuring out the lowest of the wake timestamps
		int nearestMode=0;
		for (int i=0; i<NO_POWER_MODES; i++) {
			if (savedWakeTimestamp[i]!=0 && savedWakeTimestamp[i]<savedWakeTimestamp[nearestMode]) {
				nearestMode=i;
			}
		}
		if (nearestMode==powerMode) {
			//We can wake up in this mode again.
			doSleep(canSleepForMs, nearestMode);
		} else {
			//We'll need a mode switch.
			doSleep(savedWakeTimestamp[nearestMode]-getCurrentTimeMs(), nearestMode);
		}
	}
	//No need to check power status any time soon again.
	xTimerReset(tmr, portMAX_DELAY);
}

void _powerHold(int ref, int holdTimeMs, const char *fn, const int line) {
	xSemaphoreTake(mux, portMAX_DELAY);
	PowerItem *p=findItem(ref);
	gettimeofday(&p->sleepUntil, NULL);
	p->sleepUntil.tv_sec+=holdTimeMs/1000;
	p->sleepUntil.tv_usec+=(holdTimeMs%1000)*1000;
	if (p->sleepUntil.tv_usec>1000000) {
		p->sleepUntil.tv_usec-=1000000;
		p->sleepUntil.tv_sec++;
	}
	p->state=ST_ACTIVE;
#if POWERDOWN_DBG
	p->fn=fn;
	p->line=line;
#endif
	xSemaphoreGive(mux);
}

void _powerCanSleepFor(int ref, int delayMs, const char *fn, const int line) {
	xSemaphoreTake(mux, portMAX_DELAY);
//	printf("canSleepFor %d\n", delayMs);
	PowerItem *p=findItem(ref);
	gettimeofday(&p->sleepUntil, NULL);
	p->sleepUntil.tv_sec+=delayMs/1000;
	p->sleepUntil.tv_usec+=(delayMs%1000)*1000;
	if (p->sleepUntil.tv_usec>1000000) {
		p->sleepUntil.tv_usec-=1000000;
		p->sleepUntil.tv_sec++;
	}
	p->state=ST_CANSLEEP_UNTIL;
#if POWERDOWN_DBG
	p->fn=fn;
	p->line=line;
#endif
	checkCanSleep();
	xSemaphoreGive(mux);
}

void _powerCanSleep(int ref, const char *fn, const int line) {
	xSemaphoreTake(mux, portMAX_DELAY);
	PowerItem *p=findItem(ref);
	p->state=ST_CANSLEEP;
#if POWERDOWN_DBG
	p->fn=fn;
	p->line=line;
#endif
	checkCanSleep();
	xSemaphoreGive(mux);
}

void pwrdwnmgrTimer(TimerHandle_t xTimer) {
	xSemaphoreTake(mux, portMAX_DELAY);
	checkCanSleep();
	xSemaphoreGive(mux);
}

void powerDownMgrInit(PowerDownCb *cb, void *arg, PowerMode mode, bool dbg) {
	pdCb=cb;
	pdCbArg=arg;
	mux=xSemaphoreCreateMutex();
	tmr=xTimerCreate("pwrdwnmgr", 5000/portTICK_PERIOD_MS, 1, NULL, pwrdwnmgrTimer);
	xTimerReset(tmr, portMAX_DELAY);
	xTimerStart(tmr, portMAX_DELAY);
	powerMode=mode;
	showDebugMsg=dbg;
	printf("Power down manager initialized. Mode is %d, %s\n", mode, dbg?"debug":"nodebug");
}
