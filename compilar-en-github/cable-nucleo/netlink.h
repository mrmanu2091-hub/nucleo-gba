/* GBA 2091 · Cable Link por red para el núcleo mGBA-wasm. Ver netlink.c */
#ifndef NETLINK_H
#define NETLINK_H
#include <stdbool.h>
struct mCore;
/* Lo llama main.c en loadGame(), justo después de core->reset(): si el cable está «armado», engancha el driver. */
void netlinkAttach(struct mCore* core);
#endif
