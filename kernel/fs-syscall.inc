/* Fixed 44-byte public stat layout; separate from legacy dirstat/fstat. */
struct file_stat_abi {u32 mode,size,atime,mtime,ctime,uid,gid,dev,ino,blocks,nlink;};
static u32 stat_type(u32 type) {
    return type==FS_TYPE_DIR?0040000:type==FS_TYPE_BLOCK?0060000:type==FS_TYPE_CHAR?0020000:
           type==FS_TYPE_LINK?0120000:0100000;
}
static int metadata_stat(u32 ino,struct file_stat_abi *out) {
    struct fs_stat st;int r=fs_stat(ino,&st);if(r)return fs_errno(r);
    memset(out,0,sizeof(*out));out->mode=stat_type(st.type)|st.mode;out->size=st.size;
    out->atime=st.atime;out->mtime=st.mtime;out->ctime=st.ctime;
    out->uid=st.uid;out->gid=st.gid;out->dev=(ino>>24)+1;out->ino=ino;
    out->blocks=st.blocks;out->nlink=st.links;return 0;
}
static int sys_fsctl(u32 op,u32 arg,u32 ptr) {
    struct task *p=task_process(task_current());
    if(op==9)return fs_umask(arg);
    if(op==10) {
        struct {u32 path,flags,mode;} a;
        if(!user_buf_ok(ptr,sizeof(a),false))return -EFAULT;
        memcpy(&a,(void*)ptr,sizeof(a));
        fs_namespace_lock();int r=sys_open_impl(a.path,a.flags,a.mode);fs_namespace_unlock();return r;
    }
    if(op==8){char path[PATH_MAX];if(!user_str(arg,path,sizeof(path)))return -EFAULT;return fs_errno(fs_access_path(p->cwd,path,ptr));}
    if(op==11) {
        char path[PATH_MAX];if(!user_str(arg,path,sizeof(path)))return -EFAULT;
        return fs_errno(fs_create_mode(p->cwd,path,FS_TYPE_DIR,ptr,NULL));
    }
    if(op==12) {
        char path[PATH_MAX];if(!user_str(arg,path,sizeof(path)))return -EFAULT;
        fs_namespace_lock();
        u32 ino=0;int r=fs_resolve(p->cwd,path,&ino);
        if(!r) r=fs_utime(ino,ptr);
        fs_namespace_unlock();
        return fs_errno(r);
    }
    if(op==14) {
        struct file *f=get_fd(arg);
        if(!f) return -EBADF;
        if(f->type!=FILE_DISK || !(f->mode&2)) return -EINVAL;
        fs_namespace_lock();
        int r=fs_set_size(f->ino,ptr);
        fs_namespace_unlock();
        return fs_errno(r);
    }
    if(op==13) {
        if(!user_buf_ok(ptr,16,true))return -EFAULT;
        struct fs_info info;fs_info(&info);
        u32 out[4]={info.total_blocks,info.free_blocks,info.total_inodes,info.free_inodes};
        memcpy((void*)ptr,out,16);return 0;
    }
    struct file *f=NULL;u32 ino=0;
    bool descriptor=op==3 || op==5 || op==7;
    if(descriptor){f=get_fd(arg);if(!f)return -EBADF;}
    if(op==1 || op==2 || op==3) {
        if(!user_buf_ok(ptr,sizeof(struct file_stat_abi),true))return -EFAULT;
        struct file_stat_abi out;int r=0;
        fs_namespace_lock();
        if(!descriptor) {
            char path[PATH_MAX];if(!user_str(arg,path,sizeof(path))){fs_namespace_unlock();return -EFAULT;}
            r=op==2?fs_resolve_nofollow(p->cwd,path,&ino):fs_resolve(p->cwd,path,&ino);
            r=r?fs_errno(r):metadata_stat(ino,&out);
        } else if(f->type==FILE_DISK)r=metadata_stat(f->ino,&out);
        else {
            memset(&out,0,sizeof(out));out.uid=p->uid;out.gid=p->gid;out.nlink=1;
            out.mode=(f->type==FILE_SOCKET || f->type==FILE_LOCAL?0140000:f->type==FILE_PIPE?0010000:0020000)|0600;
        }
        fs_namespace_unlock();if(!r)memcpy((void*)ptr,&out,sizeof(out));return r;
    }
    if(op<4 || op>8)return -EINVAL;
    fs_namespace_lock();int r=0;
    if(descriptor){if(f->type!=FILE_DISK)r=FS_EINVAL;else ino=f->ino;}
    else {char path[PATH_MAX];if(!user_str(arg,path,sizeof(path))){fs_namespace_unlock();return -EFAULT;}r=fs_resolve(p->cwd,path,&ino);}
    if(!r && (op==4 || op==5))r=fs_chmod(ino,ptr);
    if(!r && (op==6 || op==7)) {
        u32 ids[2];if(!user_buf_ok(ptr,8,false)){fs_namespace_unlock();return -EFAULT;}
        memcpy(ids,(void*)ptr,8);r=fs_chown(ino,ids[0],ids[1]);
    }
    if(!r && op==8)r=fs_access(ino,ptr,true);
    fs_namespace_unlock();return fs_errno(r);
}
