/* rsfile.h - Reed-Solomon protected files (.rs1 .. .rs4), EasyPal/QSSTV compatible */
#ifndef RSFILE_H
#define RSFILE_H

#include <stdint.h>

int  rs_kk(int type);                                   /* data bytes per block: 224/192/160/128 */
void rs_encode_block(uint8_t *blk, int kk);             /* 255 bytes: data[kk] then parity */
int  rs_decode_block(uint8_t *blk, int kk, const int *eras, int ne);   /* corrections, -1 failed */

/* whole file; ext = original extension ("jpg"), type 1..4 */
uint8_t *rs_file_encode(const uint8_t *data, long len, const char *ext, int type, long *outlen);
/* tr/got: received file (missing bytes may be anything), lost: 1 per byte that is missing
   (NULL: none). Returns the original file (malloc'd) and its extension, NULL if too damaged. */
uint8_t *rs_file_decode(const uint8_t *tr, long got, int type, const uint8_t *lost,
                        long *outlen, char ext[4], int *fixed);

#endif
