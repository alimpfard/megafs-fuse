// clang-format off
#include "megaclient.h"
#include "megafusemodel.h"
#include "Config.h"
#include <curl/curl.h>
#include "megacrypto.h"
#include <termios.h>
#include "megaposix.h"

#include <db_cxx.h>
#include "megabdb.h"
#include "MegaFuse.h"
#include "MAID.h"
// clang-format on
constexpr long score(long quota, int pr) { return quota * (pr ? pr : 1); }

MAID MegaFuse::findCorrespondingMaidEntry(const char *path) {
  long best = 0;
  int argbest = -1;
  for (long unsigned int i = 0; i < maidcontroller.models.size(); ++i) {
    printf("[MAID Search] searching for '%s' in idx %d...", path, i);
    if (maidcontroller.models[i]->cacheManager.find(path) !=
        maidcontroller.models[i]->cacheManager.end()) {
    ok_thats_fine:;
      auto scorev = score(maidcontroller.meta[i]->remaining_quota,
                          maidcontroller.meta[i]->priority);
      printf("exists with score %ld\n", scorev);
      if (scorev > best) {
        best = scorev;
        argbest = i;
      }
    } else {
      bool f;
      {
        std::lock_guard<std::mutex> lock2(*maidcontroller.mutexes[i]);
        f = maidcontroller.models[i]->nodeByPath(path);
      }
      if (f)
        goto ok_thats_fine;
      else
        printf("doesn't exist\n");
    }
  }
  if (argbest == -1) {
    // find the one with the highest pr
    for (int i = 0; i < maidcontroller.models.size(); ++i) {
      if (maidcontroller.meta[i]->priority >= best) {
        best = maidcontroller.meta[i]->remaining_quota;
        argbest = i;
      }
    }
  }
  if (argbest == -1)
    argbest = 0;
  return MAID{
      maidcontroller.models[argbest],
      maidcontroller.clients[argbest],
      maidcontroller.mutexes[argbest],
  };
}

std::vector<MAID> MegaFuse::findAllValidMaidEntries(const char *path) {
  std::vector<MAID> result;
  for (int i = 0; i < maidcontroller.models.size(); ++i) {
    printf("[MAID Gloabl Search] searching for '%s' in idx %d...", path, i);
    if (maidcontroller.models[i]->cacheManager.find(path) !=
        maidcontroller.models[i]->cacheManager.end()) {
    ok_thats_fine:;
      long scorev = score(maidcontroller.meta[i]->remaining_quota,
                          maidcontroller.meta[i]->priority);
      printf("exists with score %ld\n", scorev);
      result.push_back(MAID{
          maidcontroller.models[i],
          maidcontroller.clients[i],
          maidcontroller.mutexes[i],
      });
    } else {
      std::lock_guard<std::mutex> lock2(*maidcontroller.mutexes[i]);
      if (maidcontroller.models[i]->nodeByPath(path))
        goto ok_thats_fine;
      else
        printf("doesn't exist\n");
    }
  }
  return result;
}

MegaFuse::MegaFuse() : running(true) {
  event_loop_thread =
      std::thread([&](MegaFuse *v) { maidcontroller.event_loop(v); }, this);

  std::thread maintenance_loop(maintenance, this);
  maintenance_loop.detach();
  printf("MegaFS::MegaFuse. Constructor finished.\n");
}
void MegaFuse::maintenance(MegaFuse *that) {
  while (that->running) {
    if (that->needSerializeSymlink)
      that->serializeSymlink();

    if (that->needSerializeXattr)
      that->serializeXattr();

    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
  }
}

MegaFuse::~MegaFuse() {
  running = false;
  event_loop_thread.join();
  delete model;
}

bool MegaFuse::login() { return maidcontroller.login(); }

int MegaFuse::create(const char *path, mode_t mode, fuse_file_info *fi) {
  MAID maid = findCorrespondingMaidEntry(path);
  // std::lock_guard<std::mutex>lock2(engine_mutex);
  printf("----------------CREATE flags:%X\n", fi->flags);
  fi->flags = fi->flags | O_CREAT | O_TRUNC; // | O_WRONLY;

  return maid.model->open(path, fi);
}

int MegaFuse::getAttr(const char *path, struct stat *stbuf) {

  if (!symlinkLoaded)
    unserializeSymlink();

  // if (!symlinkLoaded)
  //  return -ENOSPC;
  printf("getAttr accessing '%s'\n", path);
  if (strncmp(path, "/~%", 3) == 0) {
    // dev node
    printf("Trying to access dev node? %s\n", path + 3);
    if (strcmp(path + 3, "stat") == 0) {
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = 4096;
      stbuf->st_mtime = 0;
      return 0;
    } else if (strcmp(path + 3, "pr") == 0) {
      stbuf->st_mode = S_IFREG | 0222;
      stbuf->st_nlink = 1;
      stbuf->st_size = 0;
      stbuf->st_mtime = 0;
      return 0;
  } else if (strncmp(path + 3, "resolve/", min(strlen(path+3), (size_t)8)) == 0) {
      printf("Accessing resolve:%s...\n", path+10);
      stbuf->st_mode = (strlen(path+3)>7 ? S_IFREG : S_IFDIR) | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = 4096;
      stbuf->st_mtime = 0;
      return 0;
  }
  }
  std::string str_path(path);
  MAID maid = findCorrespondingMaidEntry(path);

  std::lock_guard<std::mutex> lock2(*maid.engine_mutex);

  printf("MegaFuse::getAttr Looking for %s\n", str_path.c_str());
  if (maid.model->getAttr(path, stbuf) == 0) // file locally cached
    // just if not a symlink
    if (modelist.find(str_path) == modelist.end())
      return 0;

  Node *n = maid.model->nodeByPath(path);
  if (!n)
    return -ENOENT;

  switch (n->type) {
  case FILENODE:
    if ((modelist.find(str_path) != modelist.end())) { // symlink
      stbuf->st_mode = modelist[str_path];
    } else { // regular file
      stbuf->st_mode = S_IFREG | 0666;
    }
    stbuf->st_nlink = 1;
    stbuf->st_size = n->size;
    stbuf->st_mtime = n->ctime;
    break;

  case FOLDERNODE:
  case ROOTNODE:
    stbuf->st_mode = S_IFDIR | 0777;
    stbuf->st_nlink = 1;
    stbuf->st_size = 4096;
    stbuf->st_mtime = n->ctime;
    break;
  default:
    printf("invalid node\n");
    return -EINVAL;
  }

  return 0;
}
int MegaFuse::truncate(const char *a, off_t o) {
  fuse_file_info fi;
  fi.flags = O_TRUNC;
  if (!open(a, &fi))
    release(a, &fi);
}
int MegaFuse::mkdir(const char *p, mode_t mode) {
  MAID maid = findCorrespondingMaidEntry(p);
  std::unique_lock<std::mutex> l(*maid.engine_mutex);

  auto path = maid.model->splitPath(p);
  std::string base = path.first;
  std::string newname = path.second;

  Node *n = maid.model->nodeByPath(base.c_str());
  SymmCipher key;
  string attrstring;
  byte buf[Node::FOLDERNODEKEYLENGTH];
  NewNode *newnode = new NewNode[1];

  // set up new node as folder node
  newnode->source = NEW_NODE;
  newnode->type = FOLDERNODE;
  newnode->nodehandle = 0;
  newnode->mtime = newnode->ctime = time(NULL);
  newnode->parenthandle = UNDEF;

  // generate fresh random key for this folder node
  PrnGen::genblock(buf, Node::FOLDERNODEKEYLENGTH);
  newnode->nodekey.assign((char *)buf, Node::FOLDERNODEKEYLENGTH);
  key.setkey(buf);

  // generate fresh attribute object with the folder name
  AttrMap attrs;
  attrs.map['n'] = newname;

  // JSON-encode object and encrypt attribute string
  attrs.getjson(&attrstring);
  maid.client->makeattr(&key, &newnode->attrstring, attrstring.c_str());

  // add the newly generated folder node

  EventsListener el(maidcontroller.eh, EventsHandler::PUTNODES_RESULT);

  maid.client->putnodes(n->nodehandle, newnode, 1);
  l.unlock();
  auto l_res = el.waitEvent();
  if (l_res.result < 0)
    return -EIO;
  return 0;
}

int MegaFuse::open(const char *path, fuse_file_info *fi) {
  if (strncmp(path, "/~%", 3) == 0) {
    // dev node
    if (strcmp(path + 3, "stat") == 0) {
      return 0;
    }
    if (strcmp(path + 3, "pr") == 0) {
      return 0;
    }
    if (strncmp(path + 3, "resolve/", min((size_t) 8, strlen(path+3))) == 0)
        return 0;
  }
  MAID maid = findCorrespondingMaidEntry(path);
  return maid.model->open(path, fi);
}

int MegaFuse::read(const char *path, char *buf, size_t size, off_t offset,
                   fuse_file_info *fi) {
  // FIXME: the engine is running between the two calls
  if (strncmp(path, "/~%", 3) == 0) {
      printf("Reading dev node '%s'\n", path+3);
    // dev node
    static char vbuf[4096];
    char *fbuf = vbuf;
    if (strcmp(path + 3, "stat") == 0) {
      fbuf += sprintf(fbuf, "account stats for %d accounts\n",
                      maidcontroller.models.size());
      for (auto &model : maidcontroller.models) {
        fbuf += sprintf(fbuf, "%s:\n",
                        Config::getInstance()
                            ->userInfo[model->meta->index]
                            ->USERNAME.c_str());
        fbuf += sprintf(fbuf, "\ttotal_quota: %d\n", model->meta->total_quota);
        fbuf += sprintf(fbuf, "\tremaining_quota: %d\n",
                        model->meta->remaining_quota);
        fbuf += sprintf(fbuf, "\tpriority: %d\n", model->meta->priority);
      }
      printf("Used up %d bytes in read for stat\n", fbuf - vbuf);
      strcpy(buf, vbuf);
      return fbuf - vbuf;
    } else if (strcmp(path + 3, "pr") == 0) {
      return -ENODATA;
  } else if (strncmp(path + 3, "resolve/", 8) == 0) {
      printf("resolve %s...\n", path+10);
      MAID maid = findCorrespondingMaidEntry(path+10);
      fbuf += sprintf(fbuf, "%ld\t%s\n", maid.model->meta->index, Config::getInstance()->userInfo[maid.model->meta->index]->USERNAME.c_str());
      strcpy(buf, vbuf);
      return fbuf - vbuf;
  } else {
      printf("Unknown dev node %s\n", path+3);
      return -ENODATA;
  }
  }
  MAID maid = findCorrespondingMaidEntry(path);
  int res = maid.model->makeAvailableForRead(path, offset, size);
  if (res < 0) {
    printf("MegaFuse::read. Not available for read\n");
    return res;
  }
  return maid.model->read(path, buf, size, offset, fi);
}

int MegaFuse::readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, fuse_file_info *fi) {
  std::vector<MAID> maids = findAllValidMaidEntries(path);
  bool first = true;
  for (auto &maid : maids) {
    std::lock_guard<std::mutex> lockE(*maid.engine_mutex);
    Node *n = maid.model->nodeByPath(path);
    if (!n)
      continue;
    if (!(n->type == FOLDERNODE || n->type == ROOTNODE))
      continue;
    if (first) {
      filler(buf, ".", NULL, 0);
      filler(buf, "..", NULL, 0);
    }
    first = false;
    std::set<std::string> names = maid.model->readdir(path);

    for (node_list::iterator it = n->children.begin(); it != n->children.end();
         it++)
      names.insert((*it)->displayname());
    for (auto it = names.begin(); it != names.end(); ++it)
      filler(buf, it->c_str(), NULL, 0);
  }
  if (first)
    return -EIO;
  return 0;
}
int MegaFuse::releaseNoThumb(const char *path, fuse_file_info *fi) {
  MAID maid = findCorrespondingMaidEntry(path);
  return maid.model->release(path, fi, false);
}
int MegaFuse::release(const char *path, fuse_file_info *fi) {
  MAID maid = findCorrespondingMaidEntry(path);

  return maid.model->release(path, fi);
}

int MegaFuse::rename(const char *src, const char *dst) {
  MAID srcMaid = findCorrespondingMaidEntry(src);
  MAID dstMaid = findCorrespondingMaidEntry(dst);
  bool src_is_dst = false;

  std::unique_lock<std::mutex> lockE(*srcMaid.engine_mutex);
  if (dstMaid.engine_mutex == srcMaid.engine_mutex) {
    lockE.unlock();
    src_is_dst = true;
  }
  std::unique_lock<std::mutex> lockV(*dstMaid.engine_mutex);

  Node *n_src = srcMaid.model->nodeByPath(src);
  Node *n_dst = dstMaid.model->nodeByPath(dst);

  if (n_src) {
    auto path = dstMaid.model->splitPath(dst);
    Node *dstFolder = dstMaid.model->nodeByPath(path.first);

    if (!dstFolder)
      return -EINVAL;

    if (srcMaid.model->client->rename(n_src, dstFolder) != API_OK)
      return -EIO;

    n_src->attrs.map['n'] = path.second.c_str();
    if (srcMaid.model->client->setattr(n_src))
      return -EIO;
  } else {
    if (srcMaid.model->rename(src, dst) < 0)
      return -ENOENT;
  }
  // delete overwritten file
  if (n_dst && n_dst->type == FILENODE) {
    EventsListener el(maidcontroller.eh, EventsHandler::UNLINK_RESULT);
    if (!src_is_dst)
      lockE.unlock();
    lockV.unlock();
    dstMaid.client->unlink(n_dst);
    auto l_res = el.waitEvent();
  }

  return 0;
}
int MegaFuse::write(const char *path, const void *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
  if (strncmp(path, "/~%", 3) == 0) {
    // dev node
    static char vbuf[4096];
    char *fbuf = vbuf;
    if (strcmp(path + 3, "stat") == 0) {
      return -ENOSPC;
    } else if (strcmp(path + 3, "pr") == 0) {
      strncpy(vbuf, (const char *)buf, size);
      char vfprop[16];
      long idx;
      long value;
      if (sscanf(vbuf, "%ld.%[a-z]=%ld\n", &idx, vfprop, &value) != 3)
        return -EIO;
      if (maidcontroller.models.size() <= idx)
        return -EIO;

      MetaInformation *meta = maidcontroller.models[idx]->meta;
      if (strncmp(vfprop, "priority", min(strlen(vfprop), (size_t)8)) == 0)
        meta->priority = value;
      else if (strncmp(vfprop, "quota", min(strlen(vfprop), (size_t)5)) == 0 &&
               meta->total_quota > value)
        meta->remaining_quota = value;
      else
        return -EIO;
      return size;
    }
  }
  return write(path, (const char *)buf, size, offset, fi);
}
int MegaFuse::write(const char *path, const char *buf, size_t size,
                    off_t offset, fuse_file_info *fi) {
  if (strncmp(path, "/~%", 3) == 0) {
    // dev node
    static char vbuf[4096];
    char *fbuf = vbuf;
    if (strcmp(path + 3, "stat") == 0) {
      return -ENOSPC;
    } else if (strcmp(path + 3, "pr") == 0) {
      strncpy(vbuf, (const char *)buf, size);
      char vfprop[16];
      long idx;
      long value;
      if (sscanf(vbuf, "%ld.%[a-z]=%ld\n", &idx, vfprop, &value) != 3)
        return -EIO;
      if (maidcontroller.models.size() <= idx)
        return -EIO;

      MetaInformation *meta = maidcontroller.models[idx]->meta;
      if (strncmp(vfprop, "priority", min(strlen(vfprop), (size_t)8)) == 0)
        meta->priority = value;
      else if (strncmp(vfprop, "quota", min(strlen(vfprop), (size_t)5)) == 0 &&
               meta->total_quota > value)
        meta->remaining_quota = value;
      else
        return -EIO;
      return size;
    }
  }
  MAID maid = findCorrespondingMaidEntry(path);
  std::lock_guard<std::mutex> lock2(*maid.engine_mutex);
  return maid.model->write(path, buf, size, offset, fi);
}
int MegaFuse::unlink(const char *s) {
  printf("-----------unlink %s\n", s);
  {
    MAID maid = findCorrespondingMaidEntry(s);

    std::lock_guard<std::mutex> lock(*maid.engine_mutex);
    Node *n = maid.model->nodeByPath(s);
    if (!n)
      return maid.model->unlink(s);

    error e = maid.client->unlink(n);
    if (e)
      return -EINVAL;
  }
  {
    EventsListener el(maidcontroller.eh, EventsHandler::UNLINK_RESULT);
    auto l_res = el.waitEvent();
    if (l_res.result < 0)
      return -EIO;
    return 0;
  }

  return 0;
}
// implementation of extended attributes
int MegaFuse::setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
  needSerializeXattr = false;

  if (!xattrLoaded) // we need to prevent this
    return -ENOSPC; // it could happen while file is still
                    // downloading and we cannot access file yet

  int ret = raw_setxattr(path, name, value, size, flags);

  if (xattrLoaded) // just if we have been unserialized values
    needSerializeXattr = true;

  return ret;
}
int MegaFuse::raw_setxattr(const char *path, const char *name,
                           const char *value, size_t size, int flags) {
  std::string str_path(path);
  std::string str_name(name);

  printf("MegaFuse::raw_setxattr. setting path '%s', "
         "name '%s', value '%s' with size '%d'\n",
         path, name, value, size);
  printf("MegaFuse::raw_setxattr. setting path '%x', "
         "name '%x', value '%x' with size '%d'\n",
         path, name, value, size);

  if (xattr_list.find(str_path) != xattr_list.end()) {
    printf("MegaFuse::raw_setxattr. Path exist.\n");
    if (xattr_list[str_path].find(name) == xattr_list[str_path].end()) {
      printf("MegaFuse::raw_setxattr. "
             "name don't exist, creating.\n");
      xattr_list[str_path][str_name] = new Xattr_value;
    }
  } else {
    printf("MegaFuse::raw_setxattr. "
           "Path NO exist. Setting new value.\n");
    xattr_list[str_path][str_name] = new Xattr_value;
  }

  size_t path_t = strlen(path) + 1; // strlen gives chars count without \0
  xattr_list[str_path][str_name]->path = new char[path_t];
  memcpy(xattr_list[str_path][str_name]->path, path,
         path_t); // the null terminator should be copied

  size_t name_t = strlen(name) + 1;
  xattr_list[str_path][str_name]->name = new char[name_t];
  memcpy(xattr_list[str_path][str_name]->name, name, name_t);

  xattr_list[str_path][str_name]->value = new char[size];
  memcpy(xattr_list[str_path][str_name]->value, value, size);

  xattr_list[str_path][str_name]->size = size;

  return 0;
}
int MegaFuse::getxattr(const char *path, const char *name, char *value,
                       size_t size) {
  if (!xattrLoaded)
    unserializeXattr();

  if (!xattrLoaded)
    return -ENODATA;

  std::string str_path(path);
  std::string str_name(name);

  printf("MegaFuse::getxattr. looking for path '%s', "
         "name '%s'\n",
         path, name);
  printf("MegaFuse::getxattr. looking for path '%x', "
         "name '%x'\n",
         path, name);

  if (xattr_list.find(str_path) != xattr_list.end()) {
    printf("MegaFuse::getxattr path found, looking for name\n");
    if (xattr_list[str_path].find(str_name) != xattr_list[str_path].end()) {
      // according to doc, if size is zero, we return size of value
      // without set value
      if (size == 0) {
        return (int)xattr_list[str_path][str_name]->size;
      }

      memcpy(value, xattr_list[str_path][str_name]->value,
             (int)xattr_list[str_path][str_name]->size);

      return (int)xattr_list[str_path][str_name]->size;

    } else {
      printf("getxattr. No name '%s' "
             "found on getxattr\n",
             name);
    }
  } else {
    printf("getxattr. No path '%s'(%x) "
           "found on getxattr\n",
           path, path);
  }

  printf("getxattr. Warning. attribute "
         "not found: %s on path %s\n",
         name, path);
  return -ENODATA;
}
// implementation of symlinks
// (getAttr should return S_IFLNK byte when symlink are found)
int MegaFuse::symlink(const char *path1, const char *path2) {
  if (!symlinkLoaded)
    unserializeSymlink();

  // if (!symlinkLoaded)
  //  return -ENOSPC;

  std::string str_path2(path2);
  std::string str_path1(path1);
  unsigned int size = str_path1.size();

  mode_t mode; // not used by create func
  fuse_file_info *fi = new fuse_file_info;
  MAID maid = findCorrespondingMaidEntry(path1);

  std::unique_lock<std::mutex> lockE(*maid.engine_mutex);
  Node *n = maid.model->nodeByPath(std::string(path2));
  lockE.unlock();

  if (!(!n)) {
    // here, file exist on MEGA
    printf("MegaFuse::symlink. Removing existent file link\n");
    int res = (int)unlink(path2);
  }

  fi->flags = O_WRONLY;
  create(path2, mode, fi);

  fi->flags = O_WRONLY | O_CREAT | O_TRUNC;
  write(path2, path1, size, 0, fi);
  releaseNoThumb(path2, fi);

  printf("MegaFuse::symlink. Save %s on modelist\n", str_path2.c_str());
  modelist[str_path2] = S_IFLNK | 0666;

  needSerializeSymlink = true;

  delete fi;
  return 0;
}
int MegaFuse::readlink(const char *path, char *buf, size_t bufsiz) {
  int res;
  unsigned int size;

  char *tmpbuf = new char[bufsiz];
  memset(tmpbuf, '\0', bufsiz);

  fuse_file_info *fi = new fuse_file_info;
  fi->flags = O_CLOEXEC;
  res = (int)open(path, fi);
  fi->flags = O_RDONLY;

  res = (int)read(path, tmpbuf, bufsiz, 0, fi);
  printf("MegaFuse::readlink. read res: %d\n");

  memcpy(buf, tmpbuf, (int)bufsiz);

  printf("MegaFuse::readlink. buf size: %d\n", bufsiz);
  printf("MegaFuse::readlink. buf value: %s\n", buf);

  delete[] tmpbuf;
  delete fi;
  if (res > 0) {
    return 0;
  } else {
    return res;
  }
}
// TODO: Make it on binary mode like serializeXattr
int MegaFuse::serializeSymlink() {
  std::string sep = ";";
  std::string tmpbuf;
  std::stringstream line;

  mode_t mode; // not used by create func
  fuse_file_info *fi = new fuse_file_info;
  const char *path = "/.megafuse_symlink.temp";
  const char *path_final = "/.megafuse_symlink";
  const char *path_old = "/.megafuse_symlink.old";
  fi->flags = O_WRONLY;
  MAID maid = findCorrespondingMaidEntry(path_final);

  std::unique_lock<std::mutex> lockE(*maid.engine_mutex);
  Node *n = maid.model->nodeByPath(std::string(path_final));
  lockE.unlock();

  if (!n) {
    printf("Nothing yet here\n");
  } else {
    // here, file exist on MEGA
    printf("MegaFuse::serializeSymlink. "
           "Node EXIST on Mega? Unlink file.\n");
    rename(path_final, path_old);
  }

  fi->flags = O_WRONLY;
  create(path, mode, fi);
  fi->flags = O_APPEND;

  for (map<std::string, mode_t>::iterator iter = modelist.begin();
       iter != modelist.end(); ++iter) {
    // this could be more simple but keep in mind store also mode of file
    line << iter->first << endl;
    printf("MegaFuse::serializeSymlink. "
           "New line to serialize %s.\n",
           line.str().c_str());

    if ((tmpbuf.size() + line.str().size()) >= MEGAFUSE_BUFF_SIZE) {
      stringToFile(path, tmpbuf, tmpbuf.size(), fi);
      tmpbuf.clear(); // clear our temporal buffer
    }

    tmpbuf += line.str();
    line.clear(); // clear lines to be filled again
    line.str(std::string());
  }

  printf("MegaFuse::serializeSymlink. Calling last stringToFile\n");
  stringToFile(path, tmpbuf, tmpbuf.size(), fi);
  tmpbuf.clear();

  releaseNoThumb(path, fi);
  rename(path, path_final);

  needSerializeSymlink = false;

  delete fi;
  return 0;
}
// serialization of xattr and symlinks
int MegaFuse::serializeXattr() {
  if (maidcontroller.models.size() == 0)
    return 1;
  mode_t mode; // not used by create func
  fuse_file_info *fi = new fuse_file_info;
  const char *path = "/.megafuse_xattr.temp";
  const char *path_final = "/.megafuse_xattr";
  const char *path_old = "/.megafuse_xattr.old";
  void *binbuff = malloc(MEGAFUSE_BUFF_SIZE);
  uint32_t buffsize = 0;
  uint32_t data_size;
  fi->flags = O_WRONLY;
  for (size_t i = 0; i < maidcontroller.models.size(); i++) {
    MegaFuseModel *model = maidcontroller.models[i];
    Node *n;
    {
      std::lock_guard<std::mutex> lockG(*maidcontroller.mutexes[i]);
      n = model->nodeByPath(std::string(path_final));
    }
    if (!n) {
      // here file could not exist on MEGA but exist on cache (/tmp/mega.xxx)
      printf("MegaFuse::serializeXattr."
             "Node no exist on MEGA (could exist on cache)\n");
    } else {
      // here, file exist on MEGA. We should unserialize this?
      printf("MegaFuse::serializeXattr. "
             "Node EXIST? (on MEGA), unlink.\n");
      rename(path_final, path_old);
    }

    fi->flags = O_WRONLY;   // just to prevent error on printf on create func
    create(path, mode, fi); // this will trunk file
    fi->flags = O_APPEND;   // open file in append mode, no overwrite

    for (Map_outside::iterator iter = xattr_list.begin();
         iter != xattr_list.end(); ++iter) {
      for (Map_inside::iterator iter2 = iter->second.begin();
           iter2 != iter->second.end(); ++iter2) {
        printf("MegaFuse::serializeXatr New xattr %s "
               "on path %s "
               "with value %.*s\n",
               iter2->second->name, iter2->second->path, iter2->second->size,
               iter2->second->value);

        data_size =
            (uint32_t)((strlen(iter2->second->path) + 1)); //+1 copy also \0
        if ((buffsize + sizeof(uint32_t) + data_size) >= MEGAFUSE_BUFF_SIZE) {
          write(path, binbuff, buffsize, 0, fi);
          buffsize = 0;
        }
        addChunk(binbuff, &buffsize, iter2->second->path, data_size);

        data_size = (uint32_t)((strlen(iter2->second->name) + 1));
        if ((buffsize + sizeof(uint32_t) + data_size) >= MEGAFUSE_BUFF_SIZE) {
          write(path, binbuff, buffsize, 0, fi);
          buffsize = 0;
        }
        addChunk(binbuff, &buffsize, iter2->second->name, data_size);

        if ((buffsize + sizeof(uint32_t) + iter2->second->size) >=
            MEGAFUSE_BUFF_SIZE) {
          write(path, binbuff, buffsize, 0, fi);
          buffsize = 0;
        }
        addChunk(binbuff, &buffsize, iter2->second->value,
                 (uint32_t)iter2->second->size);
      }
    }

    printf("MegaFuse::serializeXattr: Calling stringToFile.\n");
    write(path, binbuff, buffsize, 0, fi);

    releaseNoThumb(path, fi);
    rename(path, path_final);
    releaseNoThumb(path_final, fi); // this will upload the file
  }
  free(binbuff);
  delete fi;
  needSerializeXattr = false;
  return 0;
}
uint32_t MegaFuse::addChunk(void *binbuff, uint32_t *buffsize, char *data,
                            uint32_t data_size) {
  printf("MegaFuse::addChunk  data_size:%d "
         "buffsize:%d "
         "allsize:%d\n",
         data_size, *buffsize, (*buffsize + sizeof(uint32_t) + data_size));

  printf("MegaFuse::addChunk memcpy1\n");
  memcpy(binbuff + *buffsize, &data_size, sizeof(uint32_t));

  printf("MegaFuse::addChunk memcpy2\n");
  memcpy(binbuff + *buffsize + sizeof(uint32_t), data, data_size);

  *buffsize = *buffsize + sizeof(uint32_t) + data_size;
  return *buffsize;
}
int MegaFuse::unserializeSymlink() {
  char *buffer = new char[MEGAFUSE_BUFF_SIZE];
  size_t size = MEGAFUSE_BUFF_SIZE;
  int num_byt_read = 1;
  bool isdata = false;
  fuse_file_info *fi = new fuse_file_info; // not really used
  const char *path = "/.megafuse_symlink";
  char *it = NULL;

  int sizetmp = 0;
  int err;
  off_t offset = 0;
  std::stringstream line;
  std::vector<int> sizes;

  // force download file into cache
  fi->flags = O_CLOEXEC;
  int res = (int)open(path, fi);
  fi->flags = O_RDONLY;
  if (res < 0) {
    delete[] buffer;
    delete fi;
    delete it;

    if (res == -2) { // file not found, we dont have xattr yet
      printf("MegaFuse::unserializeSymlink. "
             "no symlink stored. Do nothing.\n");
      symlinkLoaded = !symlinkLoaded;
      return res;
    }

    printf("MegaFuse::unserializeSymlink. "
           "ERROR We cannot unserialize symlinks! (%d):(\n",
           res);
    return res;
  }

  memset(buffer, '\0', MEGAFUSE_BUFF_SIZE);
  while (num_byt_read > 0) {
    memset(buffer, '\0', MEGAFUSE_BUFF_SIZE);
    num_byt_read = (int)read(path, buffer, size, offset, fi);
    // end if there is nothing to read
    if ((int)num_byt_read <= 0) {
      if ((int)num_byt_read == 0)
        symlinkLoaded = !symlinkLoaded;
      delete[] buffer;
      delete fi;
      return num_byt_read;
      break;
    }
    offset = offset + size;

    it = buffer;
    while (*it) {
      if (*it == '\n') {
        printf("MegaFuse::unserializeSymlink. "
               "Save %s on modelist\n",
               line.str().c_str());

        modelist[line.str()] = S_IFLNK | 0666;

        line.clear();
        line.str(std::string());

      } else {
        line << *it;
      }

      *it++;
    }
  }

  delete[] buffer;
  delete fi;
  return -1;
}
int MegaFuse::unserializeXattr() {
  if (xattrLoaded)
    return 0;

  char *buffer = new char[MEGAFUSE_BUFF_SIZE];
  size_t size = MEGAFUSE_BUFF_SIZE;
  int readed_b = 1;
  bool isdata = false;
  int i = 0;
  int e = 0;
  fuse_file_info *fi = new fuse_file_info; // not really used
  const char *path = "/.megafuse_xattr";
  uint32_t copied_b = 0;
  uint32_t bytes_to_copy;
  char *data[3] = {NULL}; // 0->path 1->name 2->value

  off_t offset = 0;

  // force download file into cache
  fi->flags = O_CLOEXEC;
  int res = (int)open(path, fi);
  fi->flags = O_RDONLY;
  if (res < 0) {
    delete[] buffer;
    delete fi;

    if (res == -2) { // file not found, we dont have xattr yet
      printf("MegaFuse::unserializeXattr. "
             "No xattr stored. Do nothing.\n");
      xattrLoaded = true;
      return res;
    }

    printf("MegaFuse::unserializeXattr. "
           "ERROR We cannot unserialize xattr! (%d):(\n",
           res);
    return res;
  }

  // read file in MEGAFUSE_BUFF_SIZE chunks
  for (readed_b = 1; readed_b > 0;
       readed_b = (int)read(path, buffer, size, offset, fi)) {
    // Error on data, prevent infinite loop
    // TODO: An infinite loop may occur after e=0; FIX IT
    if (e > 10) {
      break;
      printf("MegaFuse::unserializeXattr. "
             "ERROR. INCONSISTENT DATA\n");
    }
    copied_b = 0;
    while ((readed_b - copied_b) > 0 && copied_b < readed_b) {
      if (!isdata) {
        if ((readed_b - copied_b) <
            sizeof(uint32_t)) { // insufficient readed bytes
          e++;
          break; // break forces re-read file starting at offset
        }
        e = 0;

        // TODO: be carefully on little-endian vs big-endian
        memcpy(&bytes_to_copy, buffer + copied_b,
               sizeof(uint32_t)); // num of bytes to copy
        copied_b = copied_b + sizeof(uint32_t);
        isdata = !isdata;
      } else {
        if ((readed_b - copied_b) < bytes_to_copy) { // insufficient readed
                                                     // bytes
          e++;
          break; // break forces re-read file starting at offset
        }
        e = 0;

        if (data[i] != NULL)
          free(data[i]);

        data[i] = (char *)malloc(bytes_to_copy);
        memcpy(data[i], (char *)buffer + copied_b, bytes_to_copy);
        copied_b = copied_b + bytes_to_copy;

        // all data recolected 0->path 1->name 2->value
        // the last bytes_to_copy is the size of value
        if (i == 2) {
          raw_setxattr(data[0], data[1], data[2], bytes_to_copy, 0);
          i = -1;
        }
        isdata = !isdata;
        i++;

        if ((readed_b - copied_b) == 0) {
          e++;
          break;
        }
        e = 0;
      } // if-else
    }   // while
    offset = offset + copied_b;
  } // for

  delete[] buffer;
  delete fi;
  if (data[0] != NULL)
    free(data[0]);
  if (data[1] != NULL)
    free(data[1]);
  if (data[2] != NULL)
    free(data[2]);
  xattrLoaded = true;

  return 0;
}
int MegaFuse::stringToFile(const char *path, std::string tmpbuf, size_t bufsize,
                           fuse_file_info *fi) {
  if (bufsize > MEGAFUSE_BUFF_SIZE)
    return -1;

  char *buffer = new char[MEGAFUSE_BUFF_SIZE + 1];

  printf("stringToFile writting to file: %s\n", tmpbuf.c_str());
  memset(buffer, '\0', bufsize);                   // reset buffer
  memcpy(buffer, (char *)tmpbuf.c_str(), bufsize); // copy our lines to buffer
  buffer[MEGAFUSE_BUFF_SIZE] = '\0';
  write(path, buffer, tmpbuf.size(), 0,
        fi); // send the buffer to write function

  delete[] buffer;
  return 0;
}
