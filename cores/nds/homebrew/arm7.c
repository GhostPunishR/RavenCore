/*
 * Homebrew de démonstration, côté ARM7 : répond n + 1 à chaque valeur n que
 * l'ARM9 envoie par la FIFO de l'IPC (GBATEK, « DS Inter Process
 * Communication »).
 */
#include "ds.h"

int main(void) {
    IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR;
    for (;;) {
        while ((IPCFIFOCNT & FIFO_RECV_EMPTY) != 0) {
        }
        IPCFIFOSEND = IPCFIFORECV + 1u;
    }
}
