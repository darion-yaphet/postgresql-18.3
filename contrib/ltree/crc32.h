#ifndef _CRC32_H
#define _CRC32_H

/* contrib/ltree/crc32.h */

/* Returns crc32 of data block
 *
 * 返回数据块的crc32
 */
extern unsigned int ltree_crc32_sz(const char *buf, int size);

/* Returns crc32 of null-terminated string
 *
 * 返回以 null 结尾的字符串的 crc32
 */
#define crc32(buf) ltree_crc32_sz((buf),strlen(buf))

#endif
