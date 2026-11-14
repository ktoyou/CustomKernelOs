/* zlib.h — подмножество zlib, достаточное для gzip, tar, PNG и Git.
 * Поток внутри — тот же deflate, что раньше жил в user/gzip.c и user/tar.c. */
#ifndef ZLIB_H
#define ZLIB_H

#define ZLIB_VERSION "1.3.1"
#define ZLIB_VERNUM 0x1310
#define ZLIB_VER_MAJOR 1
#define ZLIB_VER_MINOR 3
#define ZLIB_VER_REVISION 1
#define ZLIB_VER_SUBREVISION 0

typedef unsigned char  Byte;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef Byte           Bytef;
typedef char           charf;
typedef int            intf;
typedef uInt           uIntf;
typedef uLong          uLongf;
typedef void          *voidpc;
typedef void          *voidpf;
typedef void          *voidp;

typedef void *(*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void  (*free_func)(voidpf opaque, voidpf address);

struct internal_state;

typedef struct z_stream_s {
    Bytef *next_in;
    uInt   avail_in;
    uLong  total_in;
    Bytef *next_out;
    uInt   avail_out;
    uLong  total_out;
    const char *msg;
    struct internal_state *state;
    alloc_func zalloc;
    free_func  zfree;
    voidpf opaque;
    int    data_type;
    uLong  adler;
    uLong  reserved;
} z_stream;

typedef z_stream *z_streamp;

#define Z_NO_FLUSH      0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH    2
#define Z_FULL_FLUSH    3
#define Z_FINISH        4
#define Z_BLOCK         5
#define Z_TREES         6

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NEED_DICT     2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_COMPRESSION         0
#define Z_BEST_SPEED             1
#define Z_BEST_COMPRESSION       9
#define Z_DEFAULT_COMPRESSION   (-1)
#define Z_DEFAULT_STRATEGY       0
#define Z_FILTERED               1
#define Z_HUFFMAN_ONLY           2
#define Z_RLE                    3
#define Z_FIXED                  4
#define Z_BINARY                 0
#define Z_TEXT                   1
#define Z_ASCII                  Z_TEXT
#define Z_UNKNOWN                2
#define Z_DEFLATED               8
#define Z_NULL                   0
#define MAX_WBITS                15
#define MAX_MEM_LEVEL            9
#define DEF_MEM_LEVEL            8

const char *zlibVersion(void);
uLong crc32(uLong crc, const Bytef *buf, uInt len);
uLong adler32(uLong adler, const Bytef *buf, uInt len);
uLong crc32_z(uLong crc, const Bytef *buf, uLong len);
uLong adler32_z(uLong adler, const Bytef *buf, uLong len);

int deflateInit_(z_streamp strm, int level, const char *version, int stream_size);
int deflateInit2_(z_streamp strm, int level, int method, int windowBits,
                  int memLevel, int strategy, const char *version, int stream_size);
int deflate(z_streamp strm, int flush);
int deflateEnd(z_streamp strm);
typedef struct gz_header_s {
    int text;
    uLong time;
    int xflags;
    int os;
    Bytef *extra;
    uInt extra_len;
    uInt extra_max;
    Bytef *name;
    uInt name_max;
    Bytef *comment;
    uInt comm_max;
    int hcrc;
    int done;
} gz_header;
typedef gz_header *gz_headerp;
int deflateSetHeader(z_streamp strm, gz_headerp head);
int deflateReset(z_streamp strm);
uLong deflateBound(z_streamp strm, uLong sourceLen);

int inflateInit_(z_streamp strm, const char *version, int stream_size);
int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size);
int inflate(z_streamp strm, int flush);
int inflateEnd(z_streamp strm);
#ifndef z_const
#define z_const const
#endif

int inflateReset(z_streamp strm);
int inflateReset2(z_streamp strm, int windowBits);

int compress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level);
uLong compressBound(uLong sourceLen);
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
int uncompress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong *sourceLen);

#define deflateInit(strm, level) \
    deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit(strm) \
    inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#define deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
    deflateInit2_((strm), (level), (method), (windowBits), (memLevel), \
                  (strategy), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit2(strm, windowBits) \
    inflateInit2_((strm), (windowBits), ZLIB_VERSION, (int)sizeof(z_stream))

#endif
