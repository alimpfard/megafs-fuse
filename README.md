## MegaFS-FUSE - MAID

A libFUSE-based userland filesystem for connecting to mega.nz which
allows a RAID-4-like configuration of accounts, with specifyable quotas and priorities


## build
```sh
$ make
```

that's it.

## run
```sh
# make a new config or modify the example one:
$ cp megafuse.conf.ex megafuse.conf
$ vi megafuse.conf

# make the mountpoint dir
$ mkdir mnt # configured in the config

# run the exec
$ ./MegaFuse

# browse
$ ls mnt
# stuff in all of the accounts

$ cat mnt/~%stat # /~% marks a dev node
# account stats for 2 accounts
# <REDACTED>:
#         total_quota: 1024000
#         remaining_quota: 1024000
#         priority: 0
# <REDACTED>:
#         total_quota: 1024000
#         remaining_quota: 1003520
#         priority: 0
#
```

## Dev nodes in the filesystem
any node starting at `/~%` from the root is a dev node, the current ones and their uses are:

| node | permissions | purpose | usage |
| :------------- | :------------- | :------------- | :------------- |
| `/~%stat`       | `-r--r--r--`  | displays the statistics | `cat ./~%stat` |
| `/~%pr`        |  `--w--w--w-` | controls properties | `echo 0.priority=4 > ./~%pr` |
| `/~%resolve`   |  `----------` | resolves a given path to an account | `cat ./~%resolve/path/to/something` |
