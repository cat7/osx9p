#ifndef VIRTIO9P_H
#define VIRTIO9P_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v9p_chan v9p_chan;

/* Find a virtio-9p device by mount tag and hold it; NULL if absent. */
v9p_chan *v9p_chan_open(const char *tag);
void v9p_chan_close(v9p_chan *ch);

/* Largest request or reply the channel can carry. */
uint32_t v9p_chan_msize(v9p_chan *ch);

/*
 * One 9P exchange. tx holds a complete T-message; the R-message is copied
 * to rx. Requests are serialised per device.
 */
int v9p_chan_rpc(v9p_chan *ch, const void *tx, uint32_t txlen,
                 void *rx, uint32_t rxmax, uint32_t *rxlen);

#ifdef __cplusplus
}
#endif

#endif
