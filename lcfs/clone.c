#include "includes.h"

/* Given a snapshot name, find its root inode */
ino_t
lc_getRootIno(struct fs *fs, const char *name, struct inode *pdir) {
    ino_t parent = fs->fs_gfs->gfs_snap_root;
    struct inode *dir = pdir ? pdir : fs->fs_gfs->gfs_snap_rootInode;
    ino_t root;

    /* Lookup parent directory in global root file system */
    if (pdir == NULL) {
        lc_inodeLock(dir, false);
    }
    root = lc_dirLookup(fs, dir, name);
    if (pdir == NULL) {
        lc_inodeUnlock(dir);
    }
    if (root == LC_INVALID_INODE) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
    } else {
        root = lc_setHandle(lc_getIndex(fs, parent, root), root);
    }
    return root;
}

/* Link shared structures from parent */
void
lc_linkParent(struct fs *fs, struct fs *pfs) {
    fs->fs_parent = pfs;
    fs->fs_bcache = pfs->fs_bcache;
    fs->fs_rfs = pfs->fs_rfs;
}

/* Create a new layer */
void
lc_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
             const char *parent, size_t size, bool rw) {
    struct fs *fs = NULL, *pfs = NULL, *rfs = NULL;
    struct inode *dir, *pdir;
    ino_t root, pinum = 0;
    struct timeval start;
    char pname[size + 1];
    bool base, init;
    size_t icsize;
    void *super;
    int err = 0;

    lc_statsBegin(&start);
    init = strstr(name, "-init") != NULL;

    /* Check if parent is specified */
    if (size) {
        memcpy(pname, parent, size);
        pname[size] = 0;
        base = false;
        icsize = init ? LC_ICACHE_SIZE_MIN : LC_ICACHE_SIZE;
    } else {
        base = true;
        assert(!init);
        icsize = LC_ICACHE_SIZE_MAX;
    }

    /* Get the global file system */
    rfs = lc_getfs(LC_ROOT_INODE, false);
    if (!lc_hasSpace(gfs, LC_LAYER_MIN_BLOCKS)) {
        err = ENOSPC;
        goto out;
    }
    root = lc_inodeAlloc(rfs);
    pdir = gfs->gfs_snap_rootInode;
    lc_inodeLock(pdir, true);
    if (!base) {
        pinum = lc_getRootIno(rfs, pname, pdir);
        if (pinum == LC_INVALID_INODE) {
            lc_inodeUnlock(pdir);
            err = ENOENT;
            goto out;
        }
    }

    /* Add the root inode to directory */
    lc_dirAdd(pdir, root, S_IFDIR, name, strlen(name));
    pdir->i_nlink++;
    lc_markInodeDirty(pdir, LC_INODE_DIRDIRTY);
    lc_updateInodeTimes(pdir, true, true);
    lc_inodeUnlock(pdir);

    /* Initialize the new layer */
    fs = lc_newFs(gfs, icsize, rw);
    lc_lock(fs, true);
    lc_mallocBlockAligned(fs, (void **)&super, LC_MEMTYPE_BLOCK);
    lc_superInit(super, 0, false);
    fs->fs_super = super;
    fs->fs_root = root;
    fs->fs_super->sb_root = root;
    fs->fs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_MOUNTED;
    if (rw) {
        fs->fs_super->sb_flags |= LC_SUPER_RDWR;
    }
    if (init) {
        fs->fs_super->sb_flags |= LC_SUPER_INIT;
    }
    if (base) {
        fs->fs_rfs = fs;
    } else {
        pfs = lc_getfs(pinum, false);
        assert(pfs->fs_pcount == 0);
        pfs->fs_frozen = true;
        assert(pfs->fs_root == lc_getInodeHandle(pinum));
        lc_linkParent(fs, pfs);
    }

    /* Add this file system to global list of file systems */
    err = lc_addfs(gfs, fs, pfs);
    if (err) {
        lc_inodeLock(pdir, true);
        lc_dirRemove(pdir, name);
        pdir->i_nlink--;
        lc_inodeUnlock(pdir);
        lc_blockFree(gfs, fs, fs->fs_sblock, 1, true);
        lc_freeLayerBlocks(gfs, fs, true, true, false);
        goto out;
    }
    fuse_reply_ioctl(req, 0, NULL, 0);
    lc_rootInit(fs, fs->fs_root);
    if (base) {
        lc_bcacheInit(fs, LC_PCACHE_SIZE, LC_PCLOCK_COUNT);
    } else {
        dir = fs->fs_rootInode;
        dir->i_flags |= LC_INODE_SHARED;

        /* Copy the parent root directory */
        lc_cloneRootDir(pfs->fs_rootInode, dir);
    }
    lc_printf("Created fs with parent %ld root %ld index %d block %ld name %s\n",
               pfs ? pfs->fs_root : -1, root, fs->fs_gindex, fs->fs_sblock, name);
#if 0
    if (fs->fs_gindex == 17) {
        ProfilerStart("/tmp/lcfs");
    }
#endif

out:
    if (err) {
        fuse_reply_err(req, err);
    }
    lc_statsAdd(rfs, LC_CLONE_CREATE, err, &start);
    if (fs) {
        if (err) {
            fs->fs_removed = true;
            lc_unlock(fs);
            lc_destroyFs(fs, true);
        } else {
            lc_unlock(fs);
        }
    }
    if (pfs) {
        lc_unlock(pfs);
    }
    lc_unlock(rfs);
}

/* Check if a layer could be removed */
static int
lc_removeLayer(struct fs *rfs, struct inode *dir, ino_t ino, bool rmdir,
               void **fsp) {
    ino_t root;

    /* There should be a file system rooted on this directory */
    root = lc_setHandle(lc_getIndex(rfs, dir->i_ino, ino), ino);
    return lc_getfsForRemoval(rfs->fs_gfs, root, (struct fs **)fsp);
}

/* Remove a layer */
void
lc_removeClone(fuse_req_t req, struct gfs *gfs, const char *name) {
    struct fs *fs = NULL, *rfs, *bfs = NULL;
    ino_t ino = gfs->gfs_snap_root;
    struct inode *pdir = NULL;
    struct timeval start;
    int err = 0;
    ino_t root;

    /* Find the inode in snapshot directory */
    lc_statsBegin(&start);
    rfs = lc_getfs(LC_ROOT_INODE, false);
    pdir = gfs->gfs_snap_rootInode;
    lc_inodeLock(pdir, true);
    err = lc_dirRemoveName(rfs, pdir, name, true,
                           (void **)&fs, lc_removeLayer);
    if (err) {
        lc_inodeUnlock(pdir);
        fuse_reply_err(req, err);
        goto out;
    }
    assert(fs->fs_removed);
    lc_inodeUnlock(pdir);
    if (fs->fs_parent) {

        /* Have the base layer locked so that that will not be deleted before
         * this layer is freed.
         */
        bfs = fs->fs_rfs;
        lc_lock(bfs, false);
    }
    fuse_reply_ioctl(req, 0, NULL, 0);
    root = fs->fs_root;

    lc_printf("Removing fs with parent %ld root %ld name %s\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1, root, name);

    /* Notify VFS about removal of a directory */
    fuse_lowlevel_notify_delete(
#ifdef FUSE3
                                gfs->gfs_se,
#else
                                gfs->gfs_ch,
#endif
                                ino, root, name, strlen(name));
    lc_invalidateDirtyPages(gfs, fs);
    lc_invalidateInodePages(gfs, fs);
    lc_invalidateInodeBlocks(gfs, fs);
    lc_blockFree(gfs, fs, fs->fs_sblock, 1, true);
    lc_freeLayerBlocks(gfs, fs, true, true, fs->fs_parent);
    lc_unlock(fs);
    lc_destroyFs(fs, true);
    if (bfs) {
        lc_unlock(bfs);
    }

out:
    lc_statsAdd(rfs, LC_CLONE_REMOVE, err, &start);
    lc_unlock(rfs);
}

/* Mount, unmount, stat a snapshot */
void
lc_snapIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
             enum ioctl_cmd cmd) {
    struct timeval start;
    struct fs *fs, *rfs;
    ino_t root;
    int err;

    lc_statsBegin(&start);
    rfs = lc_getfs(LC_ROOT_INODE, false);

    /* Unmount all layers */
    if (cmd == UMOUNT_ALL) {
        //ProfilerStop();
        fuse_reply_ioctl(req, 0, NULL, 0);
        lc_syncAllLayers(gfs);
        lc_statsAdd(rfs, LC_CLEANUP, 0, &start);
        lc_unlock(rfs);
        return;
    }
    root = lc_getRootIno(rfs, name, NULL);
    err = (root == LC_INVALID_INODE) ? ENOENT : 0;
    switch (cmd) {
    case SNAP_MOUNT:
        if (err == 0) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getfs(root, true);
            fs->fs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_MOUNTED;
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, LC_MOUNT, err, &start);
        break;

    case SNAP_STAT:
        if (err == 0) {
            fs = lc_getfs(root, false);
            fuse_reply_ioctl(req, 0, NULL, 0);
            lc_displayLayerStats(fs);
            lc_unlock(fs);
        } else {
            lc_displayStatsAll(gfs);
            fuse_reply_ioctl(req, 0, NULL, 0);
            err = 0;
        }
        lc_statsAdd(rfs, LC_STAT, err, &start);
        break;

    case SNAP_UMOUNT:
        if (err == 0) {
            fs = lc_getfs(root, false);
            if (!fs->fs_frozen &&
                (fs->fs_readOnly ||
                 (fs->fs_super->sb_flags & LC_SUPER_INIT))) {
                lc_unlock(fs);
                fs = lc_getfs(root, true);
                assert(fs->fs_snap == NULL);
                assert(!fs->fs_frozen);
                fuse_reply_ioctl(req, 0, NULL, 0);
                lc_sync(gfs, fs, false);
            } else {
                fuse_reply_ioctl(req, 0, NULL, 0);
            }
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, LC_UMOUNT, err, &start);
        break;

    case CLEAR_STAT:
        if (err == 0) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getfs(root, true);
            lc_statsDeinit(fs);
            lc_statsNew(fs);
            lc_unlock(fs);
        }
        break;

    default:
        err = EINVAL;
    }
    if (err) {
        lc_reportError(__func__, __LINE__, 0, err);
        fuse_reply_err(req, err);
    }
    lc_unlock(rfs);
}

