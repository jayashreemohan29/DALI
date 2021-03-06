// A utility file to create, attach, read and write to shared memory segments
// Change all operation shere to work on a tmp file and then rename it atomically

#include <errno.h>
#include <memory>
#include <iostream>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "posixshmem.h"
using namespace std;

namespace shm {

const string prefix = "/dev/shm/cache/";  

string create_name(string path) {
	std::replace( path.begin(), path.end(), '/', '_');	
	return path;
}

string shm_path(string name, string prefix){
	// assumes prefix ends with '/'
	return prefix + name;
}

int open_shared_file(const char* path, int flags, mode_t mode) {
	if (!path){
		errno = ENOENT;
	}
	
	flags |= O_NOFOLLOW | O_CLOEXEC;
	/* Disable asynchronous cancellation.  */
	int state;
	pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &state);
	int fd = open (path, flags, mode);
	if (fd == -1){
		cerr << "Cannot open shm segment" << endl;
	}
	pthread_setcancelstate (state, NULL);
	return fd;
}


void create_directories(string prefix, string name) {
	

}


int try_mkdir(const char* path, int mode){
	typedef struct stat stat_dir;
	stat_dir st;
	int status = 0;
	if (stat(path, &st) != 0) {
		if (mkdir(path, mode) != 0 && errno != EEXIST)
			status = -1;
	} else if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		status = -1;
	}

	return status;
}

/**
** ensures all directories in path exist
** We start working top-down to ensure
** each directory in path exists.
*/
int mkdir_path(const char *path, mode_t mode)
{
	char *pp;
	char *sp;
	int status;
	char *copypath = strdup(path);

	status = 0;
	pp = copypath;
	// Find all occurences of '/' in path : if /a/c/tmp.jpg
	// we need to mkdir(/a) and mkdir (/a/c)
	while (status == 0 && (sp = strchr(pp, '/')) != 0) {
		if (sp != pp) {
			/* Neither root nor double slash in path */
			*sp = '\0';
			status = try_mkdir(copypath, mode);
			*sp = '/';
		}
		pp = sp + 1;
	}
	if (status == 0)
		status = try_mkdir(path, mode);
	free(copypath);
	return (status);
}


int get_file_size(string filename){
	struct stat st;
	stat(filename.c_str(), &st);
	int size = st.st_size;
	return size;
}

CacheEntry::CacheEntry(string path){
	/* If tyhe path conatins prefix, 
	 * (can happen when we are attching 
	 * to an existing cache segment),  
	 * then the name of the segment is 
	 * path - prefix 
	 */
	name_ = path; 
	int found = name_.find(prefix); 
	if(found != string::npos){ 
		// Prefix is found in path
		name_.erase(found, prefix.length()); 
	}
	//name_ = path_;
	//std::replace( name_.begin(), name_.end(), '/', '_');
}

int CacheEntry::create_segment() {
	// Get the unique name for the shm segment
	//name_ =  create_name(path_);

	//Create directories in the file path if they dont exist
	// Pass only the dir heirarchy to the function. 
	// Strip off the file name
	string dir_path(name_);
	dir_path = dir_path.substr(0, dir_path.rfind("/"));
	mkdir_path((shm_path(dir_path, prefix)).c_str(), 0777);
	
	int flags = O_CREAT | O_RDWR;
	int mode = 511;
	//Get the full shm path and open it
	string shm_path_name_tmp = shm_path(name_ + "-tmp", prefix);
  //cout << "Shm path " << shm_path_name_tmp << endl;
	fd_ = open_shared_file(shm_path_name_tmp.c_str(), flags, mode);
	return fd_;
}

int CacheEntry::attach_segment(){
	// if the shm segment is already open,
	// return the descriptor
  // We could be attaching to a segment when its
  // being written to. So always try 
  // opening from the name
	//if (fd_ != -1)
  //		return fd_;

	// Else, open the file without the O_CREAT
	// flags and return the fd
	int flags = O_RDWR;
	int mode = 511;
	string shm_path_name;

	shm_path_name = shm_path(name_, prefix);

	fd_ = open_shared_file(shm_path_name.c_str(), flags, mode);	
	return fd_;
}



int CacheEntry::put_cache(string from_file) {
	int bytes_to_write = get_file_size(from_file);
	size_ = bytes_to_write;
	//cout << "will write from file " << from_file << " size " << bytes_to_write << endl;
	if (fd_ < 0){
		errno = EINVAL;
		cerr << "File " << name_ << " has invalid decriptor" << endl;
		return -1;
	}
	ftruncate(fd_, bytes_to_write);

	//mmap the shm file to get ptr
	void *ptr = nullptr;
	if ((ptr = mmap(0, bytes_to_write, PROT_WRITE, MAP_SHARED, fd_, 0)) == MAP_FAILED){
		cerr << "mmap error" << endl;  
		return -1;
	} 
	

	// write to shared memory segment
	// We will mmap the file to read from, because
	// in DALI, the file to be read will be mmaped first. 
	void *ptr_from = nullptr;
	int fd_from = -1;
	if ((fd_from = open(from_file.c_str(), O_RDONLY)) < 0) { 
		cerr << "Open failed" << endl; 
		return -1;
	}
	if ((ptr_from = mmap(0, bytes_to_write, PROT_READ, MAP_SHARED, fd_from, 0)) == MAP_FAILED){
		cerr << "mmap error" << endl; 
		return -1;
	}
	std::shared_ptr<void> p_;
	p_ = shared_ptr<void>(ptr_from, [=](void*) {
		munmap(ptr_from, bytes_to_write); 
	});
	
	//Do the memcpy now
	// memcpy(void* dest, const void* src, size)
	memcpy(ptr, p_.get(), bytes_to_write);
	//cout << "memcpy done" << endl;
	int ret = 0;

	// Now unmap both files
	if ((ret = munmap(ptr, bytes_to_write)) == -1){
		cerr << "Munmap failed" << endl;
		return -1;
	}
	
	if ((ret = munmap(ptr_from, bytes_to_write)) == -1){
		cerr << "Munmap failed" << endl;
		return -1;
	}

	close(fd_from);
  //close the tmp file
  close(fd_);

  string shm_path_name = shm_path(name_, prefix);
  string shm_path_name_tmp = shm_path(name_+"-tmp", prefix);
  //rename the file
  if ((ret = rename(shm_path_name_tmp.c_str(), shm_path_name.c_str())) < 0){
    cerr << "Caching rename failed" << endl;
    return -1;
  }

	return bytes_to_write;
}


void* CacheEntry::get_cache() {
	string from_file = prefix + name_;
	int bytes_to_read = get_file_size(from_file);
	size_ = bytes_to_read;
	//cout << "will read from file " << from_file << " size " << bytes_to_read << endl;

	// If the descriptor is invalid, you need to sttach the segment.
	if (fd_ < 0){
		errno = EINVAL;
		cerr << "File " << name_ << " has invalid decriptor" << endl;
		return nullptr;
	}
	//mmap the shm file to get ptr
	void *ptr = nullptr;   
	if ((ptr = mmap(0, bytes_to_read, PROT_READ, MAP_SHARED, fd_, 0)) == MAP_FAILED){
		cerr << "mmap error" << endl;
		return nullptr;
	}

	return ptr;
}

string CacheEntry::get_shm_path(){
	string shm_path_name; 
	shm_path_name = shm_path(name_, prefix);
	return shm_path_name;
}

int CacheEntry::close_segment(){
	int ret = 0;
	if (fd_ > -1){
		if (( ret = close(fd_)) < 0){
			cerr << "File " << prefix + name_ << " close failed" << endl;
			return -1;
		}
	}
	return 0;
}

int CacheEntry::remove_segment(){
	string shm_path_name;
	shm_path_name = shm_path(name_, prefix); 
	int result = unlink(shm_path_name.c_str());
	return result;
}

} //end namespace shm
