/* GBA 2091 · Cable Link por red para mGBA-wasm.
   Un GBASIODriver propio que, en vez de hablar con otro núcleo en la misma página (lockstep local), publica los datos
   del cable de cada fotograma y los completa cuando llegan los de los demás jugadores (ver netlink_engine.h).
   El JavaScript solo mueve mensajes pequeños por WebRTC: netlinkPoll() saca lo que hay que enviar y netlinkPush() mete lo recibido.
   Hilos: netlinkFrameEnded() corre en el hilo del juego (espera si falta algún dato); las funciones exportadas, en el hilo principal. */
#include "netlink.h"
#include "netlink_engine.h"

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#include <emscripten.h>
#include <pthread.h>
#include <time.h>

#define OUTQ 512
#define STALL_FAIL_MS 15000

static struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	NLEngine e;
	bool armed, active, failed;
	int me, n, D;
	struct GBA* gba;
	struct GBASIODriver drv;
	bool pendS; uint16_t pendVal;                 /* el padre inició una transferencia en este fotograma */
	struct { int32_t f; uint16_t v, s; uint8_t hasS, w; } out[OUTQ]; unsigned oh, ot;
	int cur[3];                                   /* mensaje sacado por netlinkPoll: a, s, p */
	uint32_t stalls, completed, maxWaitMs;
} NL = { .m = PTHREAD_MUTEX_INITIALIZER, .c = PTHREAD_COND_INITIALIZER };

/* ---------- driver SIO ---------- */
static bool d_init(struct GBASIODriver* d) { (void) d; return true; }
static void d_deinit(struct GBASIODriver* d) { (void) d; }
static void d_reset(struct GBASIODriver* d) { (void) d; }
static uint32_t d_id(const struct GBASIODriver* d) { (void) d; return 0x4E4C4B31; /* «NLK1» */ }
static bool d_load(struct GBASIODriver* d, const void* s, size_t z) { (void) d; (void) s; (void) z; return true; }
static void d_save(struct GBASIODriver* d, void** s, size_t* z) { (void) d; *s = NULL; *z = 0; }
static void d_setMode(struct GBASIODriver* d, enum GBASIOMode m) { (void) d; (void) m; }
static bool d_handles(struct GBASIODriver* d, enum GBASIOMode m) { (void) d; return NL.active && m == GBA_SIO_MULTI; }
static int d_connected(struct GBASIODriver* d) { (void) d; return NL.active ? NL.n - 1 : 0; }
static int d_deviceId(struct GBASIODriver* d) { (void) d; return NL.active ? NL.me : 0; }
static uint16_t d_wSIOCNT(struct GBASIODriver* d, uint16_t v) { (void) d; return v; }
static uint16_t d_wRCNT(struct GBASIODriver* d, uint16_t v) { (void) d; return v; }
/* El padre escribió el bit de inicio: guardamos su dato y devolvemos false = «yo cierro la transferencia» (lo hace netlinkFrameEnded) */
static bool d_start(struct GBASIODriver* d) {
	if (!NL.active || NL.me != 0) return true;
	pthread_mutex_lock(&NL.m);
	NL.pendS = true; NL.pendVal = d->p->p->memory.io[GBA_REG(SIOMLT_SEND)];
	pthread_mutex_unlock(&NL.m);
	return false;
}
static void d_finishMulti(struct GBASIODriver* d, uint16_t data[4]) { (void) d; memset(data, 0xFF, 8); }
static uint8_t d_fin8(struct GBASIODriver* d) { (void) d; return 0xFF; }
static uint32_t d_fin32(struct GBASIODriver* d) { (void) d; return 0xFFFFFFFF; }

static double nowMs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

/* ---------- fin de fotograma (hilo del juego) ---------- */
static void netlinkFrameEnded(void* ctx) {
	(void) ctx;
	if (!NL.active) return;
	struct GBASIO* sio = &NL.gba->sio;
	uint16_t data[4]; bool xfer = false, allMulti = false, took = false;

	pthread_mutex_lock(&NL.m);
	uint16_t v = NL.gba->memory.io[GBA_REG(SIOMLT_SEND)];
	int32_t f = nl_frame(&NL.e, v, NL.pendS, NL.pendVal, sio->mode == GBA_SIO_MULTI);
	NLSlot* mine = nl_slot(&NL.e, NL.me, f);
	NL.pendS = false;
	unsigned nx = (NL.oh + 1) % OUTQ;
	if (nx != NL.ot) { NL.out[NL.oh].f = f; NL.out[NL.oh].v = mine->v; NL.out[NL.oh].s = mine->s; NL.out[NL.oh].hasS = mine->hasS; NL.out[NL.oh].w = mine->w; NL.oh = nx; }

	if (!nl_ready(&NL.e)) {
		double t0 = nowMs(); NL.stalls++;
		while (NL.active && !nl_ready(&NL.e)) {
			struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 20 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
			pthread_cond_timedwait(&NL.c, &NL.m, &ts);
			double w = nowMs() - t0;
			if (w > NL.maxWaitMs) NL.maxWaitMs = (uint32_t) w;
			if (w > STALL_FAIL_MS) { NL.active = false; NL.failed = true; }     /* alguien se fue: se suelta el cable */
		}
	}
	if (NL.active) took = nl_take(&NL.e, data, &xfer, &allMulti);
	pthread_mutex_unlock(&NL.m);

	if (took && sio->mode == GBA_SIO_MULTI) {
		sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, allMulti);      /* bit «todos conectados» que miran los juegos */
		sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, allMulti);
		if (xfer) { GBASIOMultiplayerFinishTransfer(sio, data, 0); NL.completed++; }
	}
}

void netlinkAttach(struct mCore* core) {
	if (!NL.armed) return;
	pthread_mutex_lock(&NL.m);
	NL.armed = false;
	NL.gba = (struct GBA*) core->board;
	memset(&NL.drv, 0, sizeof(NL.drv));
	NL.drv.init = d_init; NL.drv.deinit = d_deinit; NL.drv.reset = d_reset; NL.drv.driverId = d_id;
	NL.drv.loadState = d_load; NL.drv.saveState = d_save; NL.drv.setMode = d_setMode; NL.drv.handlesMode = d_handles;
	NL.drv.connectedDevices = d_connected; NL.drv.deviceId = d_deviceId;
	NL.drv.writeSIOCNT = d_wSIOCNT; NL.drv.writeRCNT = d_wRCNT; NL.drv.start = d_start;
	NL.drv.finishMultiplayer = d_finishMulti; NL.drv.finishNormal8 = d_fin8; NL.drv.finishNormal32 = d_fin32;
	nl_init(&NL.e, NL.me, NL.n, NL.D);
	NL.pendS = false; NL.oh = NL.ot = 0; NL.failed = false; NL.stalls = NL.completed = NL.maxWaitMs = 0;
	NL.active = true;
	pthread_mutex_unlock(&NL.m);
	GBASIOSetDriver(&NL.gba->sio, &NL.drv);
	struct mCoreCallbacks cb = { 0 };
	cb.videoFrameEnded = netlinkFrameEnded;
	core->addCoreCallbacks(core, &cb);
}

/* ---------- API para JavaScript ---------- */
/* Arma el cable: se engancha en el próximo loadGame(). me = 0 padre (anfitrión), n = 2..4 jugadores, D = retardo en fotogramas. */
EMSCRIPTEN_KEEPALIVE int netlinkStart(int me, int n, int D) {
	NLEngine t; if (!nl_init(&t, me, n, D)) return 0;
	pthread_mutex_lock(&NL.m);
	NL.me = me; NL.n = n; NL.D = D; NL.armed = true; NL.active = false; NL.failed = false;
	pthread_mutex_unlock(&NL.m);
	return 1;
}
EMSCRIPTEN_KEEPALIVE void netlinkStop(void) {
	pthread_mutex_lock(&NL.m);
	NL.armed = false; NL.active = false;
	pthread_cond_broadcast(&NL.c);
	pthread_mutex_unlock(&NL.m);
}
/* Dato recibido de otro jugador. a = v | (w << 16) | (hasS << 17) */
EMSCRIPTEN_KEEPALIVE void netlinkPush(int p, int f, int a, int s) {
	pthread_mutex_lock(&NL.m);
	if (NL.active) { nl_receive(&NL.e, p, f, (uint16_t) a, (a >> 17) & 1, (uint16_t) s, (a >> 16) & 1); pthread_cond_broadcast(&NL.c); }
	pthread_mutex_unlock(&NL.m);
}
/* Saca el siguiente mensaje que hay que enviar: devuelve su fotograma (o -1 si no hay). Luego netlinkOut(0)=a, (1)=s, (2)=mi número */
EMSCRIPTEN_KEEPALIVE int netlinkPoll(void) {
	int r = -1;
	pthread_mutex_lock(&NL.m);
	if (NL.ot != NL.oh) {
		NL.cur[0] = NL.out[NL.ot].v | (NL.out[NL.ot].w << 16) | (NL.out[NL.ot].hasS << 17);
		NL.cur[1] = NL.out[NL.ot].s; NL.cur[2] = NL.me; r = NL.out[NL.ot].f; NL.ot = (NL.ot + 1) % OUTQ;
	}
	pthread_mutex_unlock(&NL.m);
	return r;
}
EMSCRIPTEN_KEEPALIVE int netlinkOut(int k) { return NL.cur[k < 0 ? 0 : k > 2 ? 2 : k]; }
/* 0 estado (0 apagado, 1 armado, 2 activo, 3 se perdió la conexión) · 1 esperas · 2 espera máx. ms · 3 transferencias · 4 fotograma */
EMSCRIPTEN_KEEPALIVE int netlinkStatus(int k) {
	switch (k) {
	case 0: return NL.failed ? 3 : NL.active ? 2 : NL.armed ? 1 : 0;
	case 1: return (int) NL.stalls;
	case 2: return (int) NL.maxWaitMs;
	case 3: return (int) NL.completed;
	case 4: return NL.e.f;
	}
	return 0;
}
