/* Checks apply to names and newly acquired capabilities. read/write use the
 * access mode of an already opened file, including after chmod or UID drops. */
static void credentials(u32 *uid,u32 *gid,u32 *mask,bool real_ids) {
#ifdef FS_PERMISSIONS_TEST
    extern void fs_test_credentials(u32*,u32*,u32*,bool);
    fs_test_credentials(uid,gid,mask,real_ids);
#elif defined(HOST_TEST)
    *uid=*gid=0;*mask=022;
#else
    struct task *cur=task_current(),*t=task_process(cur);
    if(!t || !t->user || cur->fs_account_scope){*uid=*gid=0;*mask=022;return;}
    *uid=t->authenticated?(real_ids?t->real_uid:t->uid):65534;
    *gid=t->authenticated?(real_ids?t->real_gid:t->gid):65534;
    *mask=t->creation_mask;
#endif
}
void fs_account_enter(void) {
#ifndef HOST_TEST
    task_current()->fs_account_scope++;
#endif
}
void fs_account_leave(void) {
#ifndef HOST_TEST
    task_current()->fs_account_scope--;
#endif
}
u32 fs_umask(u32 mask) {
#ifdef HOST_TEST
    (void)mask;return 022;
#else
    struct task *t=task_process(task_current());u32 old=t->creation_mask;t->creation_mask=mask&0777;return old;
#endif
}
static int access_impl(u32 ino,u32 mask,bool real_ids) {
    if(mask&~7u)return FS_EINVAL;
    struct fs_stat st;int ret=stat_impl(ino,&st);if(ret)return ret;
    u32 uid,gid,creation;credentials(&uid,&gid,&creation,real_ids);
    if(!uid) return (mask&1) && st.type!=FS_TYPE_DIR && !(st.mode&0111)?FS_EACCES:0;
    u32 bits=st.mode>>(uid==st.uid?6:gid==st.gid?3:0);
    return (bits&mask)==mask?0:FS_EACCES;
}
int fs_access(u32 ino,u32 mask,bool real_ids) {
    mutex_lock(&lock);int ret=access_impl(ino,mask,real_ids);mutex_unlock(&lock);return ret;
}
static int sticky_check(u32 parent,u32 ino) {
    struct fs_stat dir,entry;int ret=stat_impl(parent,&dir);if(ret)return ret;
    if(!(dir.mode&01000))return 0;
    if((ret=stat_impl(ino,&entry)))return ret;
    u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
    return !uid || uid==dir.uid || uid==entry.uid?0:FS_EPERM;
}
int fs_chmod(u32 ino,u32 mode) {
    mutex_lock(&lock);struct fs_stat st;int ret=stat_impl(ino,&st);
    u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
    if(!ret && uid && uid!=st.uid)ret=FS_EPERM;
    /* No set-ID executable semantics: reject rather than silently pretend. */
    if(!ret && (mode&~01777u))ret=FS_EINVAL;
    if(!ret && isdev(ino))ret=FS_EROFS;
    if(!ret){ret=select_inode(ino);if(!ret)ret=myfs_setattr(LOCAL(ino),mode,st.uid,st.gid);}
    mutex_unlock(&lock);return ret;
}
int fs_utime(u32 ino,u32 when) {
    mutex_lock(&lock);struct fs_stat st;int ret=stat_impl(ino,&st);
    u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
    if(!ret && uid && uid!=st.uid)ret=access_impl(ino,2,false);
    if(!ret && isdev(ino))ret=FS_EROFS;
    if(!ret){ret=select_inode(ino);if(!ret)ret=myfs_set_time(LOCAL(ino),when);}
    mutex_unlock(&lock);return ret;
}
int fs_chown(u32 ino,u32 owner,u32 group) {
    mutex_lock(&lock);struct fs_stat st;int ret=stat_impl(ino,&st);
    u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
    if(!ret && uid && (uid!=st.uid || (owner!=0xffffffffu && owner!=st.uid) ||
       (group!=0xffffffffu && group!=st.gid && group!=gid)))ret=FS_EPERM;
    if(!ret && isdev(ino))ret=FS_EROFS;
    if(!ret){ret=select_inode(ino);if(!ret)ret=myfs_setattr(LOCAL(ino),st.mode,owner==0xffffffffu?st.uid:owner,group==0xffffffffu?st.gid:group);}
    mutex_unlock(&lock);return ret;
}
