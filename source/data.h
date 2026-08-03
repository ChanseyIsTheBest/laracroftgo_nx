/* data.h -- Chaos Rings 3 data layer (loose-file AAsset + path resolution)
 *
 * CR3 ships its data as Media.Vision ".mvgl" archives plus Play Asset Delivery
 * packs, not FF4's single encrypted obb. The engine (MVGL::Utilities::Fios)
 * builds paths like "<dir>/main.10007.android.mvgl" and opens the big archives
 * with fopen, while small assets (the F0001/F0002 fonts) come through
 * AAssetManager_open + AAsset_getBuffer. We point every Android dir-path query
 * at the game folder and serve loose files from there.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __DATA_H__
#define __DATA_H__

#include <stddef.h>
#include <stdint.h>

// The absolute game directory. Never NULL. Backed by cr3_stubs.c, which returns
// the root resolved at startup by game_root.c.
const char *data_dir(void);

/* The Chaos Rings 3 layer also declared data_init(argv0),
 * data_path(name,out,outsz) and data_exists(name) here. None was ever defined
 * or called in this port, and data_path in particular collided with the
 * game_path() helper in config.h, so they are retired rather than left as
 * decoys. Use game_root.c: game_path("sub") to build a path under the game
 * folder, game_root() for the folder itself. */

// --- AAsset NDK API over loose files ----------------------------------------
struct AAssetManager;
void  *AAssetManager_fromJava(void *env, void *assetManager);
void  *AAssetManager_open(void *mgr, const char *filename, int mode);
const void *AAsset_getBuffer(void *asset);
int64_t AAsset_getLength(void *asset);
int64_t AAsset_getLength64(void *asset);
int    AAsset_read(void *asset, void *buf, size_t count);
long   AAsset_seek(void *asset, long off, int whence);
void   AAsset_close(void *asset);

#endif
