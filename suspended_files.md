# Suspended files #

A suspended file is pmemfile kept open by a process once it has closed the
pmemfile pool. This concept is very useful when multiple processes need
access to the same pmemfile pool, and efficiency is not a big issue.
Consider the example of forking a process while keeping a pmemfile resident
file open:

```
open("/pmemfilemount/a/b", O_RDWR) == 5
if (fork() == 123)
...
read(5, buffer, size)
```

In this example, a parent process can access a pmemfile resident file using fd
number 5. During fork, it should give up access to the pmemfile pool (since the
expectation is that the child process is more likely to need it) using
[pmemfile_pool_suspend](src/libpmemfile-posix/pool.c#L380). But the fd #5
is not closed from the processes point of view. The next time tries to access
the file via this fd, it needs to reopen the pool, i.e.
[resume](src/libpmemfile-posix/pool.c#L332) it. The difficulties to be solved are:

 * How to prevent the file's disappearance while the process does not have the
   pmemfile pool open? The underlying pmemfile resident file, referenced by fd #5
   in the example above should still exist, when the process resumes the pool to
   serve the read syscall.
 * How share files between a parent process, and a child process? A child process
   created by fork(2) inherits the fd numbers (execpt those opened with the
   O_CLOEXEC flag).
 * Since this obviously needs some way to store information about suspended files
   in the pmemfile pool, the question arises, how to clean up to those references
   if/when a process ends abruptly, without cleaning up its own references?

The first question is partially answered by keeping track of suspended references in
the pool (see the [suspended_references](src/libpmemfile-posix/inode.c#L708) field),
and not removing a file with non-zero ref count, even when its link count reaches
zero. This is made mode complicated by the fact, that a child process created by
fork(2) in principle starts with suspended files. It has fd numbers inherited
from a parent process, but did not yet open the corresponding pmemfile pools,
and this should be reflected in the number of suspended references associated
with those files. A solution is to increment the suspended_references field
by two before performing a fork(2), and:
  * decreasing them by one if/when the parent or the child opens one of those files
  * decreasing them immediately following a failed fork

This would allow a child process to safely inherit fd numbers from a parent process:

```
 | parent process             |
 +----------------------------+
 |                            |
 | user calls open            |
 | \open(...) = 5             |
 |  \pmemfile_open(...) = ... |
 |                            |
 | user calls fork()          |
 | |                          |
 | | pmemfile_pool_suspend()  |
 | | |\                       |
 | | | increment the          |
 | | | suspended_references   |
 | | | counter corresponding  |
 | | | to fd #5 by two        |
 | | |                        |
 | | |\closing the file       |
 | | | referenced by fd #5    |
 | | |                        |
 | | \closing the pool        |
 | |                          |
 | | execute fork()           |
 | |                          |
 | \_fork returns in parent   |      -> fork returns in child
 |                            |      |                            |
 |                            |      |                            |
 |                            |      |                            |
 |                            |      |                            |
 |                            |      |                            |
 |                            |      |                            |
 |                            |      |                            |
 |                            |      | user calls read(5, ...)    |
 | user calls read(5, ...)    |      | |\                         |
 | |\                         |      | | pmemfile_pool_resume()   |
 | | pmemfile_pool_resume()   |      | | |\_open the pool         |
 | | |\                       |      | | |                        |
 | | | open the pool          |      | | |\_open the file         |
 | | | \                      |      | | |  associated with #5    |
 | | |  block while the       |      | | |                        |
 | | |  pool is used by       |      | | \_decremenet the         |
 | | |  another process       |      | |   corresponding          |
 | | |  .                     |      | |   suspended_references   |
 | | |  .                     |      | |   counter                |
 | | |  .                     |      | |                          |
 | | |  .                     |      |  \perform                  |
 | | |  .                     |      |   pmemfile_read            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |  .                     |      |                            |
 | | |                        |      |                            |
```

In the scenario above, a child can use the pool, use fd #5, even unlink the
file, while the suspended_references counter ensures the file stays available
for any other process (the parent in this case) to access the file whenever it
manages to resume the pool. The same is true when the parent manages to resume
the pool first, and unlinks the file.

But when a process ends without cleaning up after itself, leaving suspended
references in the pmemfile pool, those files can never be removed anymore.
It is crucial to have some easy way to locate such files, and decrement
their refcount, without the cooperation of the process originally
incrementing them.

A proposal is that libpmemfile should create links to these open files (or
open directories) right before suspending the pool, in a special directory.
This special directory has its own mount point (e.g. /proc/pmemfile).

To clean up after ill fated processes, one can access this special directory,
list the links, and decrement the suspended reference counter via these links.

The following example show a way to look at the references used by a process
with pid 1234, and clean them up:
```sh
$ pmemfile_mount /home/user/the_pmemfile_pool /mnt/pmemfile
$ pmemfile_mount --proc /home/user/the_pmemfile_pool /proc/pmemfile
$ ./buggy_program /mnt/pmemfile/xy
Segmentation fault (core dumped)
$ ls -l /proc/pmemfile
dr-xr-xr-x 2 user user 0 Sep  6 10:55 1234
$ ls -l /proc/pmemfile/1234/fd
total 0
lrwx------ 1 user user 64 Sep 19 11:58 56
lrwx------ 1 user user 64 Sep 19 11:59 58
$ cat /proc/pmemfile/1234/fd/56
Some data the program was writing, but did not fi
$ unlink /proc/pmemfile/1234/56
```

These links to suspended files would not be actual hardlinks, but special
references, specific to pmemfile, but visible to users as regular links.
The `rm` command in the example above would call `unlink(2)`, which would
be forwarded to `pmemfile_unlink`, which in turn would need to recognize
this. The easiest way to implement this special link feature is to have
a special flag for this in the on-media inode struct. When pmemfile_unlink
is called to unlink an entry from a directory, whose inode has this flag set,
it shell decrement the suspended reference counter, instead of the
regular (user visible) link count. The underlying file should only disappear,
when both counters reached zero.

To make this more general, and easier to implement/test/maintain, we would
introduce the conecpt of namespaces. Each inode would have several link counts.
Instead of having a field called [nlink](src/libpmemfile-posix/layout.h#L170)
and a field called [suspended_references](src/libpmemfile-posix/layout.h#L158),
an array of integeres would be stored in a field named `nlink`.

```c
struct pmemfile_inode {
	...
	uint64_t nlink[NAMESPACE_COUNT];
	...
	unsigned namespace_index; /* which namespace this inode actually belongs to */
};
```

Each link count would specify the number of links from a particular namespace.
Each namespace would also have its own root inode in a [superblock](src/libpmemfile-posix/layout.h#L227):

```c
struct pmemfile_super {
	...
	TOID(struct pmemfile_inode) root_inode[NAMESPACE_COUNT];
	...
};
```

The following rules should be followed:
  * An inode being created inherits the `namespace_index` field from its parent
  * It is not allowed to create a link from one namespace to another using pmemfile_link
  * The only way to create such a special link, is via a pmemfile-posix API, using a
    function called pmemfile_linkat_cross_namespace. This should prevent most users from
    accidentally creating such links, as that would be impossible using the LD_PRELOAD
    mechanism.
  * When pmemfile_unlink, or pmemfile_unlinkat is called on such a link (linked from one
    namespace to another), it should decrease the link count referring to the parent's
    namespace. e.g.:
    ```c
    pmemfile_open(pfp_client, "/a/b", O_CREAT) /* creats an inode, where .namespace_index == 0 ; .nlink[0] == 1 ; .nlink[1] == 0 */
    dir_a = pmemfile_open(pfp_client, "/a", O_DIRECTORY); /* opens the directory, whose .namespace_index = 0 */
    dir_p = pmemfile_open(pfp_proc, "/pmemfile/1234/fd", O_DIRECTORY); /* opens the directory, whose .namespace_index = 1 */
    pmemfile_linkat_cross_namespace(pfp_proc, dir_a, "b", dir_b, "6"); /* creates the link: /proc/pmemfile/1234/fd/6" */
    /*
     * This results in a situation where the directory "/proc/1234/fd" has .namespace_index = 1 with a entry pointing
     * to an inode with .namespace_index == 0. This inode has .nlink[0] == 1 ; and .nlink[1] == 1, thus is referenced by
     * both namespaces.
     */
    ```

But the problems around fork(2) are still not solved. In this scenario, when a fork happens two
references should exist in the special proc namespace, e.g. if the parent has pid 123, and the child
has pid 124, and an fd #5 was inherited by the child, the following references should be present
right after fork:
```
/proc/pmemfile/123/fd/5
/proc/pmemfile/124/fd/5
```

Both references should be created before performing the fork, but at that point, the parent
does not yet know the pid of its future child. Therefore it uses a temporary name:

  * Before fork
    ```
    /proc/pmemfile/123/fd/5
    /proc/pmemfile/123_0/fd/5
    ```
  * After fork, the parent process attempts to rename the path component `123_0`
    to `124`, to make it match child's pid:
    ```
    /proc/pmemfile/123/fd/5
    /proc/pmemfile/124/fd/5
    ```
    This can succeed, if the child did not resume the pool yet.
    If the child already resumed the pool, then it is the child's respondibility
    to remove these temporary references, or to rename `123_0` to `124`.

### execve ###

  A process created using execve inherits file descriptions without the
FD_CLOEXEC flag set. This is simply not supported in by pmemfile, and all files
should be closed before an execve call, which also means releaseing all
suspended files. The exec family of syscalls are usually called right after
forking a process. This means, a process calling execve might easily have
references to suspended files, and this process should reopen the pmemfile
pool in order to release these files, before actually performing execve.
   There are two problems with this scenario:
   1) If the execve call fails, the process still expects those files
      to be available. This should not be a big problem, since a failed
      execve is usually just followed by an exit syscall, it is a very rare
      situation where a process would use an fd after execve.
   3) An execve can happen in a process created using vfork, and in this
      case, reopening the pool and releasing the files would mess with
      data in the parent processes address space.
