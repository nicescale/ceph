#ifndef __CLIENT_H
#define __CLIENT_H

//#include "mds/MDCluster.h"
#include "osd/OSDMap.h"

#include "msg/Message.h"
#include "msg/Dispatcher.h"
#include "msg/Messenger.h"
#include "msg/SerialMessenger.h"

#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"

//#include "msgthread.h"

#include "include/types.h"
#include "include/lru.h"
#include "include/filepath.h"
#include "include/rangeset.h"

#include "common/Mutex.h"

// stl
#include <set>
#include <map>
using namespace std;

#include <ext/hash_map>
using namespace __gnu_cxx;


class Filer;

extern class LogType client_logtype;
extern class Logger  *client_logger;



// ============================================
// types for my local metadata cache
/* basic structure:
   
 - Dentries live in an LRU loop.  they get expired based on last access.
      see include/lru.h.  items can be bumped to "mid" or "top" of list, etc.
 - Inode has ref count for each Fh, Dir, or Dentry that points to it.
 - when Inode ref goes to 0, it's expired.
 - when Dir is empty, it's removed (and it's Inode ref--)
 
*/

typedef int fh_t;

class Dir;
class Inode;

class Dentry : public LRUObject {
 public:
  string  name;                      // sort of lame
  //const char *name;
  Dir     *dir;
  Inode   *inode;
  int     ref;                       // 1 if there's a dir beneath me.
  
  void get() { assert(ref == 0); ref++; lru_pin(); }
  void put() { assert(ref == 1); ref--; lru_unpin(); }
  
  Dentry() : dir(0), inode(0), ref(0) { }
  /*Dentry() : name(0), dir(0), inode(0), ref(0) { }
  Dentry(string& n) : name(0), dir(0), inode(0), ref(0) { 
	name = new char[n.length()+1];
	strcpy((char*)name, n.c_str());
  }
  ~Dentry() {
	delete[] name;
	}*/
};

class Dir {
 public:
  Inode    *parent_inode;  // my inode
  //hash_map<const char*, Dentry*, hash<const char*>, eqstr> dentries;
  hash_map<string, Dentry*> dentries;

  Dir(Inode* in) { parent_inode = in; }

  bool is_empty() {  return dentries.empty(); }
};


class InodeCap {
 public:
  int  caps;
  long seq;
  InodeCap() : caps(0), seq(0) {}
};


class Inode {
 public:
  inode_t   inode;    // the actual inode
  int       mds_dir_auth;
  set<int>	mds_contacts;
  time_t    last_updated;

  // per-mds caps
  map<int,InodeCap> caps;            // mds -> InodeCap
  map<int,InodeCap> stale_caps;      // mds -> cap .. stale

  time_t    file_wr_mtime;   // [writers] time of last write
  off_t     file_wr_size;    // [writers] largest offset we've written to
  int       num_rd, num_wr;  // num readers, writers

  int       ref;      // ref count. 1 for each dentry, fh that links to me.
  Dir       *dir;     // if i'm a dir.
  Dentry    *dn;      // if i'm linked to a dentry.
  string    *symlink; // symlink content, if it's a symlink

  list<Cond*>       waitfor_write;
  list<Cond*>       waitfor_read;
  list<Cond*>       waitfor_flushed;
  set<bufferlist*>  inflight_buffers;

  void get() { ref++; }
  void put() { ref--; assert(ref >= 0); }

  Inode() : mds_dir_auth(-1), last_updated(0),
	file_wr_mtime(0), file_wr_size(0), num_rd(0), num_wr(0),
	ref(0), dir(0), dn(0), symlink(0) { }
  ~Inode() {
	if (symlink) { delete symlink; symlink = 0; }
  }

  inodeno_t ino() { return inode.ino; }

  bool is_dir() {
	return (inode.mode & INODE_TYPE_MASK) == INODE_MODE_DIR;
  }

  int file_caps() {
	int c = 0;
	for (map<int,InodeCap>::iterator it = caps.begin();
		 it != caps.end();
		 it++)
	  c |= it->second.caps;
	for (map<int,InodeCap>::iterator it = stale_caps.begin();
		 it != stale_caps.end();
		 it++)
	  c |= it->second.caps;
	return c;
  }

  int file_caps_wanted() {
	int w = 0;
	if (num_rd) w |= CAP_FILE_RD|CAP_FILE_RDCACHE;
	if (num_wr) w |= CAP_FILE_WR|CAP_FILE_WRBUFFER;
	return w;
  }

  int authority() {
	// my info valid?
	if (mds_dir_auth >= 0)  
	  return mds_dir_auth;
	
	// otherwise try parent
	if (dn && dn->dir && dn->dir->parent_inode) 
	  return dn->dir->parent_inode->authority();

	return 0;  // who knows!
  }
  set<int>& get_replicas() {
	if (mds_contacts.size())
	  return mds_contacts;
	if (is_dir()) {
	  return mds_contacts;
	} 
	if (dn && dn->dir && dn->dir->parent_inode) {
	  return dn->dir->parent_inode->get_replicas();
	}
	return mds_contacts;
  }
  

  // open Dir for an inode.  if it's not open, allocated it (and pin dentry in memory).
  Dir *open_dir() {
	if (!dir) {
	  if (dn) dn->get();  	// pin dentry
	  get();
	  dir = new Dir(this);
	}
	return dir;
  }
};




// file handle for any open file state

struct Fh {
  Inode    *inode;
  int       mds;        // have to talk to mds we opened with (for now)
  int       mode;       // the mode i opened the file with
};





// ========================================================
// client interface

class Client : public Dispatcher {
 protected:
  Messenger *messenger;  
  int whoami;
  
  // cluster descriptors
  //MDCluster             *mdcluster; 
  OSDMap                *osdmap;

  bool   mounted;
  bool   unmounting;
  Cond   unmount_cond;  
  
  Filer                 *filer;  // (non-blocking) osd interface
  
  // cache
  hash_map<inodeno_t, Inode*> inode_map;
  Inode*                 root;
  LRU                    lru;    // lru list of Dentry's in our local metadata cache.

  // cap weirdness
  map<inodeno_t, map<int, class MClientFileCaps*> > cap_reap_queue;  // ino -> mds -> msg .. set of (would-be) stale caps to reap


  // file handles
  rangeset<fh_t>         free_fh_set;  // unused fh's
  hash_map<fh_t, Fh*>    fh_map;
  
  fh_t get_fh() {
	fh_t fh = free_fh_set.first();
	free_fh_set.erase(fh);
	return fh;
  }
  void put_fh(fh_t fh) {
	free_fh_set.insert(fh);
  }


  // global client lock
  //  - protects Client and buffer cache both!
  Mutex                  client_lock;


  // -- metadata cache stuff

  // decrease inode ref.  delete if dangling.
  void put_inode(Inode *in) {
	in->put();
	if (in->ref == 0) {
	  inode_map.erase(in->inode.ino);
	  if (in == root) root = 0;
	  delete in;
	}
  }

  void close_dir(Dir *dir) {
	assert(dir->is_empty());
	
	Inode *in = dir->parent_inode;
	if (in->dn) in->dn->put();   // unpin dentry
	
	delete in->dir;
	in->dir = 0;
	put_inode(in);
  }

  int get_cache_size() { return lru.lru_get_size(); }
  void set_cache_size(int m) { lru.lru_set_max(m); }

  Dentry* link(Dir *dir, string& name, Inode *in) {
	Dentry *dn = new Dentry;
	dn->name = name;
	
	// link to dir
	dn->dir = dir;
	dir->dentries[dn->name] = dn;

	// link to inode
	dn->inode = in;
	in->dn = dn;
	in->get();

	lru.lru_insert_mid(dn);    // mid or top?
	return dn;
  }

  void unlink(Dentry *dn) {
	Inode *in = dn->inode;

	// unlink from inode
	dn->inode = 0;
	in->dn = 0;
	put_inode(in);
	
	// unlink from dir
	dn->dir->dentries.erase(dn->name);
	if (dn->dir->is_empty()) 
	  close_dir(dn->dir);
	dn->dir = 0;

	// delete den
	lru.lru_remove(dn);
	delete dn;
  }

  Dentry *relink(Dentry *dn, Dir *dir, string& name) {
	// first link new dn to dir
	/*
	char *oldname = (char*)dn->name;
	dn->name = new char[name.length()+1];
	strcpy((char*)dn->name, name.c_str());
	dir->dentries[dn->name] = dn;
	*/
	dir->dentries[name] = dn;

	// unlink from old dir
	dn->dir->dentries.erase(dn->name);
	//delete[] oldname;
	if (dn->dir->is_empty()) 
	  close_dir(dn->dir);

	// fix up dn
	dn->name = name;
	dn->dir = dir;

	return dn;
  }

  // move dentry to top of lru
  void touch_dn(Dentry *dn) { lru.lru_touch(dn); }  

  // trim cache.
  void trim_cache();
  void dump_inode(Inode *in, set<Inode*>& did);
  void dump_cache();  // debug
  
  // find dentry based on filepath
  Dentry *lookup(filepath& path);


  // blocking mds call
  MClientReply *make_request(MClientRequest *req, bool auth_best=false, int use_auth=-1);

  
  // -- buffer cache --
  class Buffercache *bc;
  
  void flush_buffers(int ttl, off_t dirty_size);     // flush dirty buffers
  void trim_bcache();
  void flush_inode_buffers(Inode *in);     // flush buffered writes
  void release_inode_buffers(Inode *in);   // release cached reads
  void tear_down_bcache();
		

  // friends
  friend class SyntheticClient;

 public:
  Client(Messenger *m);
  ~Client();
  void tear_down_cache();   

  int get_nodeid() { return whoami; }

  void init();
  void shutdown();

  // messaging
  void dispatch(Message *m);

  // file caps
  void handle_file_caps(class MClientFileCaps *m);
  void release_caps(Inode *in, int retain=0);
  void update_caps_wanted(Inode *in);

  // metadata cache
  Inode* insert_inode_info(Dir *dir, c_inode_info *in_info);
  void insert_trace(const vector<c_inode_info*>& trace);

  // ----------------------
  // fs ops.
  int mount(int mkfs=0);
  int unmount();

  // these shoud (more or less) mirror the actual system calls.
  int statfs(const char *path, struct statfs *stbuf);

  // namespace ops
  int getdir(const char *path, map<string,inode_t*>& contents);
  int link(const char *existing, const char *newname);
  int unlink(const char *path);
  int rename(const char *from, const char *to);

  // dirs
  int mkdir(const char *path, mode_t mode);
  int rmdir(const char *path);

  // symlinks
  int readlink(const char *path, char *buf, off_t size);
  int symlink(const char *existing, const char *newname);

  // inode stuff
  int lstat(const char *path, struct stat *stbuf);
  int chmod(const char *path, mode_t mode);
  int chown(const char *path, uid_t uid, gid_t gid);
  int utime(const char *path, struct utimbuf *buf);
  
  // file ops
  int mknod(const char *path, mode_t mode);
  int open(const char *path, int mode);
  int close(fh_t fh);
  int read(fh_t fh, char *buf, off_t size, off_t offset);
  int write(fh_t fh, const char *buf, off_t size, off_t offset);
  int truncate(const char *file, off_t size);
	//int truncate(fh_t fh, long long size);
  int fsync(fh_t fh, bool syncdataonly);

};

#endif
