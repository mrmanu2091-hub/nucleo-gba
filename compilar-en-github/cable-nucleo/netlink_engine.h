/* GBA 2091 · Motor del cable Link en C (sin dependencias de mGBA ni de hilos: se prueba solo).
   Misma idea que linkengine.js: cada jugador publica por fotograma {v: SIOMLT_SEND, s: valor de arranque del padre,
   w: ¿está en modo multijugador?}. Una transferencia iniciada en el fotograma g se completa en TODOS justo antes
   del fotograma g+D con los datos de S{g}. Un jugador no empieza el fotograma f hasta tener S{f-D} de todos. */
#ifndef NETLINK_ENGINE_H
#define NETLINK_ENGINE_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NL_MAX 4
#define NL_RING 64          /* debe ser potencia de 2 y > 2*D+4 (D <= 24) */
#define NL_MAXD 24

typedef struct { int32_t fr; uint16_t v, s; uint8_t hasS, w; } NLSlot;
typedef struct {
	int me, n, D;
	int32_t f;               /* último fotograma terminado por este jugador */
	int32_t done;            /* último g ya completado */
	NLSlot h[NL_MAX][NL_RING];
} NLEngine;

static inline NLSlot* nl_slot(NLEngine* e, int p, int32_t f) { return &e->h[p][f & (NL_RING - 1)]; }
static inline bool nl_have(NLEngine* e, int p, int32_t f) { return nl_slot(e, p, f)->fr == f; }

static inline bool nl_init(NLEngine* e, int me, int n, int D) {
	if (n < 2 || n > NL_MAX || me < 0 || me >= n || D < 1 || D > NL_MAXD) return false;
	memset(e, 0, sizeof(*e));
	e->me = me; e->n = n; e->D = D; e->f = -1; e->done = -1;
	for (int p = 0; p < NL_MAX; ++p) for (int i = 0; i < NL_RING; ++i) e->h[p][i].fr = -1;
	return true;
}

/* El jugador terminó un fotograma. Devuelve el número de fotograma publicado. */
static inline int32_t nl_frame(NLEngine* e, uint16_t v, bool hasS, uint16_t s, bool multi) {
	e->f++;
	NLSlot* sl = nl_slot(e, e->me, e->f);
	sl->fr = e->f; sl->v = v; sl->hasS = (e->me == 0 && hasS) ? 1 : 0; sl->s = sl->hasS ? s : 0; sl->w = multi ? 1 : 0;
	return e->f;
}

/* Llega el dato de otro jugador (duplicados y fotogramas fuera de rango se ignoran) */
static inline void nl_receive(NLEngine* e, int p, int32_t f, uint16_t v, bool hasS, uint16_t s, bool multi) {
	if (p < 0 || p >= e->n || p == e->me || f < 0) return;
	if (f < e->done - 2 || f > e->f + e->D + 2) return;
	NLSlot* sl = nl_slot(e, p, f);
	if (sl->fr == f) return;
	sl->fr = f; sl->v = v; sl->hasS = (p == 0 && hasS) ? 1 : 0; sl->s = sl->hasS ? s : 0; sl->w = multi ? 1 : 0;
}

/* ¿se puede empezar el fotograma f+1? */
static inline bool nl_ready(NLEngine* e) {
	int32_t g = e->f + 1 - e->D;
	if (g < 0) return true;
	for (int p = 0; p < e->n; ++p) if (!nl_have(e, p, g)) return false;
	return true;
}

/* Llamar UNA vez por fotograma cuando nl_ready() es true. Rellena data[4] si hay transferencia que completar.
   *allMulti = todos los jugadores estaban en modo multijugador en S{g} (para el bit «Ready» del cable). */
static inline bool nl_take(NLEngine* e, uint16_t data[4], bool* xfer, bool* allMulti) {
	*xfer = false; *allMulti = false;
	int32_t g = e->f + 1 - e->D;
	if (g < 0 || g <= e->done) return false;
	e->done = g;
	bool all = true;
	for (int p = 0; p < e->n; ++p) all = all && nl_slot(e, p, g)->w;
	*allMulti = all;
	NLSlot* par = nl_slot(e, 0, g);
	if (par->hasS) {
		data[0] = par->s;
		for (int p = 1; p < NL_MAX; ++p) data[p] = p < e->n ? nl_slot(e, p, g)->v : 0xFFFF;
		*xfer = true;
	}
	return true;
}

/* D recomendado a partir del RTT máximo medido (ms) */
static inline int nl_pickD(int rttMs, int n, int mode /* -1 rápido, 0 auto, 1 estable */) {
	double hops = n > 2 ? 1.0 : 0.5, frame = 1000.0 / 59.7275;
	double x = (rttMs * hops + 25.0) / frame; int d = (int) x; if (x > d) d++;
	d += 1 + (mode < 0 ? -1 : mode > 0 ? 3 : 0);
	if (d < 2) { d = 2; }
	if (d > 14) { d = 14; }
	return d;
}
#endif
