/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2010, 2011, 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#include "ruby.h"
#ifdef HAVE_RUBY_IO_H
	/* Ruby 1.9 */
	#include "ruby/intern.h"
	#include "ruby/io.h"
#else
	#include "rubysig.h"
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <grp.h>
#include <signal.h>
#include <pthread.h>
#ifdef HAVE_ALLOCA_H
	#include <alloca.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	#define HAVE_KQUEUE
	#include <sys/event.h>
	#include <sys/time.h>
#endif

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#ifndef RARRAY_LEN
	#define RARRAY_LEN(ary) RARRAY(ary)->len
#endif
#ifndef RSTRING_PTR
	#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif
#ifndef RSTRING_LEN
	#define RSTRING_LEN(str) RSTRING(str)->len
#endif
#if !defined(RUBY_UBF_IO) && defined(RB_UBF_DFL)
	/* MacRuby compatibility */
	#define RUBY_UBF_IO RB_UBF_DFL
#endif
#ifndef IOV_MAX
	/* Linux doesn't define IOV_MAX in limits.h for some reason. */
	#define IOV_MAX sysconf(_SC_IOV_MAX)
#endif

static VALUE mPassenger;
static VALUE mNativeSupport;
static VALUE S_ProcessTimes;
#ifdef HAVE_KQUEUE
	static VALUE cFileSystemWatcher;
#endif
static VALUE cDeadlineTimer;

/*
 * call-seq: disable_stdio_buffering
 *
 * Disables any kind of buffering on the C +stdout+ and +stderr+ variables,
 * so that +fprintf()+ on +stdout+ and +stderr+ have immediate effect.
 */
static VALUE
disable_stdio_buffering(VALUE self) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	return Qnil;
}

/**
 * Split the given string into an hash. Keys and values are obtained by splitting the
 * string using the null character as the delimitor.
 */
static VALUE
split_by_null_into_hash(VALUE self, VALUE data) {
	const char *cdata   = RSTRING_PTR(data);
	unsigned long len   = RSTRING_LEN(data);
	const char *begin   = cdata;
	const char *current = cdata;
	const char *end     = cdata + len;
	VALUE result, key, value;
	
	result = rb_hash_new();
	while (current < end) {
		if (*current == '\0') {
			key   = rb_str_substr(data, begin - cdata, current - begin);
			begin = current = current + 1;
			while (current < end) {
				if (*current == '\0') {
					value = rb_str_substr(data, begin - cdata, current - begin);;
					begin = current = current + 1;
					rb_hash_aset(result, key, value);
					break;
				} else {
					current++;
				}
			}
		} else {
			current++;
		}
	}
	return result;
}

typedef struct {
	/* The IO vectors in this group. */
	struct iovec *io_vectors;
	
	/* The number of IO vectors in io_vectors. */
	unsigned int count;
	
	/* The combined size of all IO vectors in this group. */
	ssize_t      total_size;
} IOVectorGroup;

/* Given that _bytes_written_ bytes in _group_ had been successfully written,
 * update the information in _group_ so that the next writev() call doesn't
 * write the already written bytes.
 */
static void
update_group_written_info(IOVectorGroup *group, ssize_t bytes_written) {
	unsigned int i;
	size_t counter;
	struct iovec *current_vec;
	
	/* Find the last vector that contains data that had already been written. */
	counter = 0;
	for (i = 0; i < group->count; i++) {
		counter += group->io_vectors[i].iov_len;
		if (counter == (size_t) bytes_written) {
			/* Found. In fact, all vectors up to this one contain exactly
			 * bytes_written bytes. So discard all these vectors.
			 */
			group->io_vectors += i + 1;
			group->count -= i + 1;
			group->total_size -= bytes_written;
			return;
		} else if (counter > (size_t) bytes_written) {
			/* Found. Discard all vectors before this one, and
			 * truncate this vector.
			 */
			group->io_vectors += i;
			group->count -= i;
			group->total_size -= bytes_written;
			current_vec = &group->io_vectors[0];
			current_vec->iov_base = ((char *) current_vec->iov_base) +
				current_vec->iov_len - (counter - bytes_written);
			current_vec->iov_len = counter - bytes_written;
			return;
		}
	}
	rb_raise(rb_eRuntimeError, "writev() returned an unexpected result");
}

#ifndef TRAP_BEG
	typedef struct {
		int filedes;
		const struct iovec *iov;
		int iovcnt;
	} WritevWrapperData;
	
	static VALUE
	writev_wrapper(void *ptr) {
		WritevWrapperData *data = (WritevWrapperData *) ptr;
		return (VALUE) writev(data->filedes, data->iov, data->iovcnt);
	}
#endif

static VALUE
f_generic_writev(VALUE fd, VALUE *array_of_components, unsigned int count) {
	VALUE components, str;
	unsigned int total_size, total_components, ngroups;
	IOVectorGroup *groups;
	unsigned int i, j, group_offset, vector_offset;
	unsigned long long ssize_max;
	ssize_t ret;
	int done, fd_num, e;
	#ifndef TRAP_BEG
		WritevWrapperData writev_wrapper_data;
	#endif
	
	/* First determine the number of components that we have. */
	total_components   = 0;
	for (i = 0; i < count; i++) {
	        Check_Type(array_of_components[i], T_ARRAY);
		total_components += (unsigned int) RARRAY_LEN(array_of_components[i]);
	}
	if (total_components == 0) {
		return NUM2INT(0);
	}
	
	/* A single writev() call can only accept IOV_MAX vectors, so we
	 * may have to split the components into groups and perform
	 * multiple writev() calls, one per group. Determine the number
	 * of groups needed, how big each group should be and allocate
	 * memory for them.
	 */
	if (total_components % IOV_MAX == 0) {
		ngroups = total_components / IOV_MAX;
		groups  = alloca(ngroups * sizeof(IOVectorGroup));
		if (groups == NULL) {
			rb_raise(rb_eNoMemError, "Insufficient stack space.");
		}
		memset(groups, 0, ngroups * sizeof(IOVectorGroup));
		for (i = 0; i < ngroups; i++) {
			groups[i].io_vectors = alloca(IOV_MAX * sizeof(struct iovec));
			if (groups[i].io_vectors == NULL) {
				rb_raise(rb_eNoMemError, "Insufficient stack space.");
			}
			groups[i].count = IOV_MAX;
		}
	} else {
		ngroups = total_components / IOV_MAX + 1;
		groups  = alloca(ngroups * sizeof(IOVectorGroup));
		if (groups == NULL) {
			rb_raise(rb_eNoMemError, "Insufficient stack space.");
		}
		memset(groups, 0, ngroups * sizeof(IOVectorGroup));
		for (i = 0; i < ngroups - 1; i++) {
			groups[i].io_vectors = alloca(IOV_MAX * sizeof(struct iovec));
			if (groups[i].io_vectors == NULL) {
				rb_raise(rb_eNoMemError, "Insufficient stack space.");
			}
			groups[i].count = IOV_MAX;
		}
		groups[ngroups - 1].io_vectors = alloca((total_components % IOV_MAX) * sizeof(struct iovec));
		if (groups[ngroups - 1].io_vectors == NULL) {
			rb_raise(rb_eNoMemError, "Insufficient stack space.");
		}
		groups[ngroups - 1].count = total_components % IOV_MAX;
	}
	
	/* Now distribute the components among the groups, filling the iovec
	 * array in each group. Also calculate the total data size while we're
	 * at it.
	 */
	total_size    = 0;
	group_offset  = 0;
	vector_offset = 0;
	for (i = 0; i < count; i++) {
		components = array_of_components[i];
		for (j = 0; j < (unsigned int) RARRAY_LEN(components); j++) {
			str = rb_ary_entry(components, j);
			str = rb_obj_as_string(str);
			total_size += (unsigned int) RSTRING_LEN(str);
			/* I know writev() doesn't write to iov_base, but on some
			 * platforms it's still defined as non-const char *
			 * :-(
			 */
			groups[group_offset].io_vectors[vector_offset].iov_base = (char *) RSTRING_PTR(str);
			groups[group_offset].io_vectors[vector_offset].iov_len  = RSTRING_LEN(str);
			groups[group_offset].total_size += RSTRING_LEN(str);
			vector_offset++;
			if (vector_offset == groups[group_offset].count) {
				group_offset++;
				vector_offset = 0;
			}
		}
	}
	
	/* We don't compare to SSIZE_MAX directly in order to shut up a compiler warning on OS X Snow Leopard. */
	ssize_max = SSIZE_MAX;
	if (total_size > ssize_max) {
		rb_raise(rb_eArgError, "The total size of the components may not be larger than SSIZE_MAX.");
	}
	
	/* Write the data. */
	fd_num = NUM2INT(fd);
	for (i = 0; i < ngroups; i++) {
		/* Wait until the file descriptor becomes writable before writing things. */
		rb_thread_fd_writable(fd_num);
		
		done = 0;
		while (!done) {
			#ifdef TRAP_BEG
				TRAP_BEG;
				ret = writev(fd_num, groups[i].io_vectors, groups[i].count);
				TRAP_END;
			#else
				writev_wrapper_data.filedes = fd_num;
				writev_wrapper_data.iov     = groups[i].io_vectors;
				writev_wrapper_data.iovcnt  = groups[i].count;
				ret = (int) rb_thread_blocking_region(writev_wrapper,
					&writev_wrapper_data, RUBY_UBF_IO, 0);
			#endif
			if (ret == -1) {
				/* If the error is something like EAGAIN, yield to another
				 * thread until the file descriptor becomes writable again.
				 * In case of other errors, raise an exception.
				 */
				if (!rb_io_wait_writable(fd_num)) {
					rb_sys_fail("writev()");
				}
			} else if (ret < groups[i].total_size) {
				/* Not everything in this group has been written. Retry without
				 * writing the bytes that been successfully written.
				 */
				e = errno;
				update_group_written_info(&groups[i], ret);
				errno = e;
				rb_io_wait_writable(fd_num);
			} else {
				done = 1;
			}
		}
	}
	return INT2NUM(total_size);
}

/**
 * Writes all of the strings in the +components+ array into the given file
 * descriptor using the +writev()+ system call. Unlike IO#write, this method
 * does not require one to concatenate all those strings into a single buffer
 * in order to send the data in a single system call. Thus, #writev is a great
 * way to perform zero-copy I/O.
 *
 * Unlike the raw writev() system call, this method ensures that all given
 * data is written before returning, by performing multiple writev() calls
 * and whatever else is necessary.
 *
 *   writev(@socket.fileno, ["hello ", "world", "\n"])
 */
static VALUE
f_writev(VALUE self, VALUE fd, VALUE components) {
	return f_generic_writev(fd, &components, 1);
}

/**
 * Like #writev, but accepts two arrays. The data is written in the given order.
 *
 *   writev2(@socket.fileno, ["hello ", "world", "\n"], ["another ", "message\n"])
 */
static VALUE
f_writev2(VALUE self, VALUE fd, VALUE components1, VALUE components2) {
	VALUE array_of_components[2] = { components1, components2 };
	return f_generic_writev(fd, array_of_components, 2);
}

/**
 * Like #writev, but accepts three arrays. The data is written in the given order.
 *
 *   writev3(@socket.fileno,
 *     ["hello ", "world", "\n"],
 *     ["another ", "message\n"],
 *     ["yet ", "another ", "one", "\n"])
 */
static VALUE
f_writev3(VALUE self, VALUE fd, VALUE components1, VALUE components2, VALUE components3) {
	VALUE array_of_components[3] = { components1, components2, components3 };
	return f_generic_writev(fd, array_of_components, 3);
}

static VALUE
process_times(VALUE self) {
	struct rusage usage;
	unsigned long long utime, stime;
	
	if (getrusage(RUSAGE_SELF, &usage) == -1) {
		rb_sys_fail("getrusage()");
	}
	
	utime = (unsigned long long) usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec;
	stime = (unsigned long long) usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
	return rb_struct_new(S_ProcessTimes, rb_ull2inum(utime), rb_ull2inum(stime));
}

static void *
detach_process_main(void *arg) {
	pid_t pid = (pid_t) (long) arg;
	int ret;
	do {
		ret = waitpid(pid, NULL, 0);
	} while (ret == -1 && errno == EINTR);
	return NULL;
}

static VALUE
detach_process(VALUE self, VALUE pid) {
	pthread_t thr;
	pthread_attr_t attr;
	size_t stack_size = 96 * 1024;
	
	unsigned long min_stack_size;
	int stack_min_size_defined;
	int round_stack_size;
	
	#ifdef PTHREAD_STACK_MIN
		// PTHREAD_STACK_MIN may not be a constant macro so we need
		// to evaluate it dynamically.
		min_stack_size = PTHREAD_STACK_MIN;
		stack_min_size_defined = 1;
	#else
		// Assume minimum stack size is 128 KB.
		min_stack_size = 128 * 1024;
		stack_min_size_defined = 0;
	#endif
	if (stack_size != 0 && stack_size < min_stack_size) {
		stack_size = min_stack_size;
		round_stack_size = !stack_min_size_defined;
	} else {
		round_stack_size = 1;
	}
	
	if (round_stack_size) {
		// Round stack size up to page boundary.
		long page_size;
		#if defined(_SC_PAGESIZE)
			page_size = sysconf(_SC_PAGESIZE);
		#elif defined(_SC_PAGE_SIZE)
			page_size = sysconf(_SC_PAGE_SIZE);
		#elif defined(PAGESIZE)
			page_size = sysconf(PAGESIZE);
		#elif defined(PAGE_SIZE)
			page_size = sysconf(PAGE_SIZE);
		#else
			page_size = getpagesize();
		#endif
		if (stack_size % page_size != 0) {
			stack_size = stack_size - (stack_size % page_size) + page_size;
		}
	}
	
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, 1);
	pthread_attr_setstacksize(&attr, stack_size);
	pthread_create(&thr, &attr, detach_process_main, (void *) NUM2LONG(pid));
	pthread_attr_destroy(&attr);
	return Qnil;
}

/**
 * Freeze the current process forever. On Ruby 1.9 this never unlocks the GIL.
 * Useful for testing purposes.
 */
static VALUE
freeze_process(VALUE self) {
	while (1) {
		usleep(60 * 1000000);
	}
	return Qnil;
}

#if defined(HAVE_KQUEUE) || defined(IN_DOXYGEN)
typedef struct {
	VALUE klass;
	VALUE filenames;
	VALUE termination_pipe;
	
	/* File descriptor of termination_pipe. */
	int termination_fd;
	
	/* Whether something went wrong during initialization. */
	int preparation_error;
	
	/* Information for kqueue. */
	unsigned int events_len;
	int *fds;
	unsigned int fds_len;
	int kq;
	
	/* When the watcher thread is done it'll write to this pipe
	 * to signal the main (Ruby) thread.
	 */
	int notification_fd[2];
	
	/* When the main (Ruby) thread is interrupted it'll write to
	 * this pipe to tell the watcher thread to exit.
	 */
	int interruption_fd[2];
} FSWatcher;

typedef struct {
	int fd;
	ssize_t ret;
	char byte;
	int error;
} FSWatcherReadByteData;

static void
fs_watcher_real_close(FSWatcher *watcher) {
	unsigned int i;
	
	if (watcher->kq != -1) {
		close(watcher->kq);
		watcher->kq = -1;
	}
	if (watcher->notification_fd[0] != -1) {
		close(watcher->notification_fd[0]);
		watcher->notification_fd[0] = -1;
	}
	if (watcher->notification_fd[1] != -1) {
		close(watcher->notification_fd[1]);
		watcher->notification_fd[1] = -1;
	}
	if (watcher->interruption_fd[0] != -1) {
		close(watcher->interruption_fd[0]);
		watcher->interruption_fd[0] = -1;
	}
	if (watcher->interruption_fd[1] != -1) {
		close(watcher->interruption_fd[1]);
		watcher->interruption_fd[1] = -1;
	}
	if (watcher->fds != NULL) {
		for (i = 0; i < watcher->fds_len; i++) {
			close(watcher->fds[i]);
		}
		free(watcher->fds);
		watcher->fds = NULL;
		watcher->fds_len = 0;
	}
}

static void
fs_watcher_free(void *obj) {
	FSWatcher *watcher = (FSWatcher *) obj;
	fs_watcher_real_close(watcher);
	free(watcher);
}

static VALUE
fs_watcher_init(VALUE arg) {
	FSWatcher *watcher = (FSWatcher *) arg;
	struct kevent *events;
	VALUE filename;
	unsigned int i;
	uint32_t fflags;
	VALUE filenum;
	struct stat buf;
	int fd;
	
	/* Open each file in the filenames list and add each one to the events array. */
	
	/* +2 for the termination pipe and the interruption pipe. */
	events = alloca((RARRAY_LEN(watcher->filenames) + 2) * sizeof(struct kevent));
	watcher->fds = malloc(RARRAY_LEN(watcher->filenames) * sizeof(int));
	if (watcher->fds == NULL) {
		rb_raise(rb_eNoMemError, "Cannot allocate memory.");
		return Qnil;
	}
	for (i = 0; i < RARRAY_LEN(watcher->filenames); i++) {
		filename = rb_ary_entry(watcher->filenames, i);
		if (TYPE(filename) != T_STRING) {
			filename = rb_obj_as_string(filename);
		}
		
		if (stat(RSTRING_PTR(filename), &buf) == -1) {
			watcher->preparation_error = 1;
			goto end;
		}
		
		#ifdef O_EVTONLY
			fd = open(RSTRING_PTR(filename), O_EVTONLY);
		#else
			fd = open(RSTRING_PTR(filename), O_RDONLY);
		#endif
		if (fd == -1) {
			watcher->preparation_error = 1;
			goto end;
		}
		
		watcher->fds[i] = fd;
		watcher->fds_len++;
		fflags = NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE;
		EV_SET(&events[i], fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
			fflags, 0, 0);
	}
	
	watcher->events_len = watcher->fds_len;
	
	/* Create pipes for inter-thread communication. */
	
	if (pipe(watcher->notification_fd) == -1) {
		rb_sys_fail("pipe()");
		return Qnil;
	}
	if (pipe(watcher->interruption_fd) == -1) {
		rb_sys_fail("pipe()");
		return Qnil;
	}
	
	/* Create a kqueue and register all events. */
	
	watcher->kq = kqueue();
	if (watcher->kq == -1) {
		rb_sys_fail("kqueue()");
		return Qnil;
	}
	
	if (watcher->termination_pipe != Qnil) {
		filenum = rb_funcall(watcher->termination_pipe,
			rb_intern("fileno"), 0);
		EV_SET(&events[watcher->events_len], NUM2INT(filenum),
			EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, 0);
		watcher->termination_fd = NUM2INT(filenum);
		watcher->events_len++;
	}
	EV_SET(&events[watcher->events_len], watcher->interruption_fd[0],
		EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, 0);
	watcher->events_len++;
	
	if (kevent(watcher->kq, events, watcher->events_len, NULL, 0, NULL) == -1) {
		rb_sys_fail("kevent()");
		return Qnil;
	}
	
end:
	if (watcher->preparation_error) {
		for (i = 0; i < watcher->fds_len; i++) {
			close(watcher->fds[i]);
		}
		free(watcher->fds);
		watcher->fds = NULL;
		watcher->fds_len = 0;
	}
	return Data_Wrap_Struct(watcher->klass, NULL, fs_watcher_free, watcher);
}

static VALUE
fs_watcher_new(VALUE klass, VALUE filenames, VALUE termination_pipe) {
	FSWatcher *watcher;
	VALUE result;
	int status;
	
	Check_Type(filenames, T_ARRAY);
	watcher = (FSWatcher *) calloc(1, sizeof(FSWatcher));
	if (watcher == NULL) {
		rb_raise(rb_eNoMemError, "Cannot allocate memory.");
		return Qnil;
	}
	watcher->klass = klass;
	watcher->filenames = filenames;
	watcher->termination_pipe = termination_pipe;
	watcher->termination_fd = -1;
	watcher->kq = -1;
	watcher->notification_fd[0] = -1;
	watcher->notification_fd[1] = -1;
	watcher->interruption_fd[0] = -1;
	watcher->interruption_fd[1] = -1;
	
	result = rb_protect(fs_watcher_init, (VALUE) watcher, &status);
	if (status) {
		fs_watcher_free(watcher);
		rb_jump_tag(status);
		return Qnil;
	} else {
		return result;
	}
}

static void *
fs_watcher_wait_on_kqueue(void *arg) {
	FSWatcher *watcher = (FSWatcher *) arg;
	struct kevent *events;
	int nevents;
	ssize_t ret;
	
	events = alloca(sizeof(struct kevent) * watcher->events_len);
	nevents = kevent(watcher->kq, NULL, 0, events, watcher->events_len, NULL);
	if (nevents == -1) {
		ret = write(watcher->notification_fd[1], "e", 1);
	} else if (nevents >= 1 && (
		   events[0].ident == (uintptr_t) watcher->termination_fd
		|| events[0].ident == (uintptr_t) watcher->interruption_fd[0]
	)) {
		ret = write(watcher->notification_fd[1], "t", 1);
	} else {
		ret = write(watcher->notification_fd[1], "f", 1);
	}
	if (ret == -1) {
		close(watcher->notification_fd[1]);
		watcher->notification_fd[1] = -1;
	}
	return NULL;
}

static VALUE
fs_watcher_wait_fd(VALUE _fd) {
	int fd = (int) _fd;
	rb_thread_wait_fd(fd);
	return Qnil;
}

#ifndef TRAP_BEG
	static VALUE
	fs_watcher_read_byte_from_fd_wrapper(void *_arg) {
		FSWatcherReadByteData *data = (FSWatcherReadByteData *) _arg;
		data->ret = read(data->fd, &data->byte, 1);
		data->error = errno;
		return Qnil;
	}
#endif

static VALUE
fs_watcher_read_byte_from_fd(VALUE _arg) {
	FSWatcherReadByteData *data = (FSWatcherReadByteData *) _arg;
	#ifdef TRAP_BEG
		TRAP_BEG;
		data->ret = read(data->fd, &data->byte, 1);
		TRAP_END;
		data->error = errno;
	#else
		rb_thread_blocking_region(fs_watcher_read_byte_from_fd_wrapper,
			data, RUBY_UBF_IO, 0);
	#endif
	return Qnil;
}

static VALUE
fs_watcher_wait_for_change(VALUE self) {
	FSWatcher *watcher;
	pthread_t thr;
	ssize_t ret;
	int e, interrupted = 0;
	FSWatcherReadByteData read_data;
	
	Data_Get_Struct(self, FSWatcher, watcher);
	
	if (watcher->preparation_error) {
		return Qfalse;
	}
	
	/* Spawn a thread, and let the thread perform the blocking kqueue
	 * wait. When kevent() returns the thread will write its status to the
	 * notification pipe. In the mean time we let the Ruby interpreter wait
	 * on the other side of the pipe for us so that we don't block Ruby
	 * threads.
	 */
	
	e = pthread_create(&thr, NULL, fs_watcher_wait_on_kqueue, watcher);
	if (e != 0) {
		errno = e;
		rb_sys_fail("pthread_create()");
		return Qnil;
	}
	
	/* Note that rb_thread_wait() does not wait for the fd when the app
	 * is single threaded, so we must join the thread after we've read
	 * from the notification fd.
	 */
	rb_protect(fs_watcher_wait_fd, (VALUE) watcher->notification_fd[0], &interrupted);
	if (interrupted) {
		/* We got interrupted so tell the watcher thread to exit. */
		ret = write(watcher->interruption_fd[1], "x", 1);
		if (ret == -1) {
			e = errno;
			fs_watcher_real_close(watcher);
			errno = e;
			rb_sys_fail("write() to interruption pipe");
			return Qnil;
		}
		pthread_join(thr, NULL);
		
		/* Now clean up stuff. */
		fs_watcher_real_close(watcher);
		rb_jump_tag(interrupted);
		return Qnil;
	}
	
	read_data.fd = watcher->notification_fd[0];
	rb_protect(fs_watcher_read_byte_from_fd, (VALUE) &read_data, &interrupted);
	if (interrupted) {
		/* We got interrupted so tell the watcher thread to exit. */
		ret = write(watcher->interruption_fd[1], "x", 1);
		if (ret == -1) {
			e = errno;
			fs_watcher_real_close(watcher);
			errno = e;
			rb_sys_fail("write() to interruption pipe");
			return Qnil;
		}
		pthread_join(thr, NULL);
		
		/* Now clean up stuff. */
		fs_watcher_real_close(watcher);
		rb_jump_tag(interrupted);
		return Qnil;
	}
	
	pthread_join(thr, NULL);
	
	if (read_data.ret == -1) {
		fs_watcher_real_close(watcher);
		errno = read_data.error;
		rb_sys_fail("read()");
		return Qnil;
	} else if (read_data.ret == 0) {
		fs_watcher_real_close(watcher);
		errno = read_data.error;
		rb_raise(rb_eRuntimeError, "Unknown error: unexpected EOF");
		return Qnil;
	} else if (read_data.byte == 't') {
		/* termination_fd or interruption_fd became readable */
		return Qnil;
	} else if (read_data.byte == 'f') {
		/* a file or directory changed */
		return Qtrue;
	} else {
		fs_watcher_real_close(watcher);
		errno = read_data.error;
		rb_raise(rb_eRuntimeError, "Unknown error: unexpected notification data");
		return Qnil;
	}
}

static VALUE
fs_watcher_close(VALUE self) {
	FSWatcher *watcher;
	Data_Get_Struct(self, FSWatcher, watcher);
	fs_watcher_real_close(watcher);
	return Qnil;
}
#endif


/***************************/

typedef struct {
	pthread_t thr;
	int channel[2];
} DeadlineTimer;

#define THREAD_STACK_SIZE (128 * 1024)

static void deadline_timer_free(void *p);
static void *deadline_timer_thread_main(void *p);

static VALUE
deadline_timer_new(VALUE klass) {
	DeadlineTimer *dt;
	VALUE result;
	pthread_attr_t attr;
	int ret;
	unsigned int min_stack_size;
	
	dt = (DeadlineTimer *) malloc(sizeof(DeadlineTimer));
	if (dt == NULL) {
		rb_raise(rb_eNoMemError, "Cannot allocate memory.");
		return Qnil;
	}
	
	if (pipe(dt->channel) != 0) {
		int e = errno;
		free(dt);
		errno = e;
		rb_sys_fail("Cannot create a pipe");
		return Qnil;
	}
	
	ret = pthread_attr_init(&attr);
	if (ret != 0) {
		close(dt->channel[0]);
		close(dt->channel[1]);
		free(dt);
		rb_raise(rb_eRuntimeError, "Cannot initialize thread attributes.");
		return Qnil;
	}
	#ifdef PTHREAD_STACK_MIN
		min_stack_size = PTHREAD_STACK_MIN;
	#else
		min_stack_size = 64 * 1024;
	#endif
	if (THREAD_STACK_SIZE < min_stack_size) {
		pthread_attr_setstacksize(&attr, min_stack_size);
	} else {
		pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
	}
	
	ret = pthread_create(&dt->thr, NULL, deadline_timer_thread_main, dt);
	pthread_attr_destroy(&attr);
	if (ret != 0) {
		close(dt->channel[0]);
		close(dt->channel[1]);
		free(dt);
		errno = ret;
		rb_sys_fail("Cannot create a new thread");
		return Qnil;
	}
	
	result = Data_Wrap_Struct(klass, 0, deadline_timer_free, dt);
	rb_obj_call_init(result, 0, NULL);
	return result;
}

static void *
deadline_timer_thread_main(void *p) {
	DeadlineTimer *dt = (DeadlineTimer *) p;
	fd_set set;
	int timeout, ret;
	struct timeval tv;
	ssize_t size;
	char tmp;
	int done = 0;
	
	while (!done) {
		do {
			size = read(dt->channel[0], &timeout, sizeof(timeout));
		} while (size == -1 && errno == EINTR);
		if (size == -1) {
			ret = errno;
			fprintf(stderr, "*** Passenger::DeadlineTimer thread: cannot read from channel: %s (%d)\n",
				strerror(ret), ret);
			fflush(stderr);
			done = 1;
		} else if (size == 0) {
			done = 1;
		} else {
			FD_ZERO(&set);
			FD_SET(dt->channel[0], &set);
			tv.tv_sec = timeout;
			tv.tv_usec = 0;
			
			do {
				ret = select(dt->channel[0] + 1, &set, NULL, NULL, &tv);
			} while (ret == -1 && errno == EINTR);
			if (ret == 0) {
				fprintf(stderr,
					"*** Passenger: Killing process %ld because "
					"its deadline of %d seconds has expired.\n",
					(long) getpid(),
					timeout);
				fflush(stderr);
				do {
					ret = kill(getpid(), SIGKILL);
				} while (ret == -1 && errno == EINTR);
			} else if (ret == -1) {
				ret = errno;
				fprintf(stderr, "*** Passenger::DeadlineTimer "
					"thread: cannot call select() on channel: %s (%d)\n",
					strerror(ret), ret);
				fflush(stderr);
				done = 1;
			} else {
				// Read single byte and discard it.
				do {
					size = read(dt->channel[0], &tmp, 1);
				} while (size == -1 && errno == EINTR);
			}
		}
	}
	return NULL;
}

static VALUE
deadline_timer_init(VALUE self) {
	/* A no-op. */
	return Qnil;
}

static VALUE
deadline_timer_start(VALUE self, VALUE timeout) {
	DeadlineTimer *dt;
	int timeout_value;
	ssize_t size;
	
	Data_Get_Struct(self, DeadlineTimer, dt);
	timeout_value = NUM2INT(timeout);
	do {
		size = write(dt->channel[1], &timeout_value, sizeof(timeout_value));
	} while (size == -1 && errno == EINTR);
	if (size == -1) {
		rb_sys_fail("Cannot write to pipe");
	}
	return Qnil;
}

static VALUE
deadline_timer_stop(VALUE self) {
	DeadlineTimer *dt;
	ssize_t size;
	
	Data_Get_Struct(self, DeadlineTimer, dt);
	do {
		size = write(dt->channel[1], "x", 1);
	} while (size == -1 && errno == EINTR);
	if (size == -1) {
		rb_sys_fail("Cannot write to pipe");
	}
	return Qnil;
}

static VALUE
deadline_timer_cleanup(VALUE self) {
	DeadlineTimer *dt;
	int ret;
	
	Data_Get_Struct(self, DeadlineTimer, dt);
	do {
		ret = close(dt->channel[1]);
	} while (ret == -1 && errno == EINTR);
	do {
		ret = pthread_join(dt->thr, NULL);
	} while (ret == EINTR);
	do {
		ret = close(dt->channel[0]);
	} while (ret == -1 && errno == EINTR);
	
	dt->channel[0] = -1;
	
	return Qnil;
}

static void
deadline_timer_free(void *p) {
	DeadlineTimer *dt = (DeadlineTimer *) p;
	int ret;
	
	if (dt->channel[0] != -1) {
		do {
			ret = close(dt->channel[1]);
		} while (ret == -1 && errno == EINTR);
		do {
			ret = pthread_join(dt->thr, NULL);
		} while (ret == EINTR);
		do {
			ret = close(dt->channel[0]);
		} while (ret == -1 && errno == EINTR);
	}
	free(dt);
}

void
Init_passenger_native_support() {
	struct sockaddr_un addr;
	
	/* */
	mPassenger = rb_define_module("PhusionPassenger"); // Do not remove the above comment. We want the Passenger module's rdoc to be empty.
	
	/*
	 * Utility functions for accessing system functionality.
	 */
	mNativeSupport = rb_define_module_under(mPassenger, "NativeSupport");
	
	S_ProcessTimes = rb_struct_define("ProcessTimes", "utime", "stime", NULL);
	
	rb_define_singleton_method(mNativeSupport, "disable_stdio_buffering", disable_stdio_buffering, 0);
	rb_define_singleton_method(mNativeSupport, "split_by_null_into_hash", split_by_null_into_hash, 1);
	rb_define_singleton_method(mNativeSupport, "writev", f_writev, 2);
	rb_define_singleton_method(mNativeSupport, "writev2", f_writev2, 3);
	rb_define_singleton_method(mNativeSupport, "writev3", f_writev3, 4);
	rb_define_singleton_method(mNativeSupport, "process_times", process_times, 0);
	rb_define_singleton_method(mNativeSupport, "detach_process", detach_process, 1);
	rb_define_singleton_method(mNativeSupport, "freeze_process", freeze_process, 0);
	
	#ifdef HAVE_KQUEUE
		cFileSystemWatcher = rb_define_class_under(mNativeSupport,
			"FileSystemWatcher", rb_cObject);
		rb_define_singleton_method(cFileSystemWatcher, "_new",
			fs_watcher_new, 2);
		rb_define_method(cFileSystemWatcher, "wait_for_change",
			fs_watcher_wait_for_change, 0);
		rb_define_method(cFileSystemWatcher, "close",
			fs_watcher_close, 0);
	#endif
	
	/* The maximum length of a Unix socket path, including terminating null. */
	rb_define_const(mNativeSupport, "UNIX_PATH_MAX", INT2NUM(sizeof(addr.sun_path)));
	/* The maximum size of the data that may be passed to #writev. */
	rb_define_const(mNativeSupport, "SSIZE_MAX", LL2NUM(SSIZE_MAX));
	
	/*
	 * A deadline timer is used to abort the current process if an operation
	 * takes too much time. Example usage:
	 *
	 *   timer = Passenger::DeadlineTimer.new
	 *   timer.start(5)   # Set deadline to 5 seconds.
	 *   begin
	 *      # If timer.stop isn't called within 5 seconds, then the
	 *      # current process will be killed by SIGKILL.
	 *      do_something
	 *   ensure
	 *      timer.stop
	 *   end
	 *   timer.cleanup
	 *
	 * Be very careful with using this class. It does not perform a lot
	 * of error checking. A #start call *must* be followed by a #stop call.
	 * #cleanup *must* be called then the timer is no longer needed.
	 */
	cDeadlineTimer = rb_define_class_under(mPassenger, "DeadlineTimer", rb_cObject);
	rb_define_singleton_method(cDeadlineTimer, "new", deadline_timer_new, 0);
	rb_define_method(cDeadlineTimer, "initialize", deadline_timer_init, 0);
	rb_define_method(cDeadlineTimer, "start", deadline_timer_start, 1);
	rb_define_method(cDeadlineTimer, "stop", deadline_timer_stop, 0);
	rb_define_method(cDeadlineTimer, "cleanup", deadline_timer_cleanup, 0);
}
