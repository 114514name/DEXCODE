/* ============================================================================
 * ds_embed.h — 内嵌的前端资源(B7)。真正的数据在生成的 ds_embed.c 里。
 * ==========================================================================*/
#ifndef DS_EMBED_H
#define DS_EMBED_H

typedef struct {
    const char *name;             /* 文件名(如 "index.html") */
    const unsigned char *data;
    unsigned size;
} DsAsset;

/* 所有资源内容的哈希:变了就说明前端资源变了,缓存目录要换一个 */
const char *ds_embed_hash(void);
int ds_embed_count(void);
const DsAsset *ds_embed_at(int i);

#endif /* DS_EMBED_H */
