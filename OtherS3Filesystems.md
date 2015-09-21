Here is a list of other projects that use Amazon S3 to create a Linux filesystem:

  * [s3ql](http://code.google.com/p/s3ql/): open source S3 backed file system optimized for backups
  * [s3fs](http://code.google.com/p/s3fs/): file-oriented filesystem backed by S3 written in C++
  * [ElasticDrive](http://www.elasticdrive.com/): a commercial "distributed remove storage application" capable of using S3
  * [PersistentFS](http://www.persistentfs.com/): an S3-backed filesystem, seems to be non open-source.
  * [s3fs](https://fedorahosted.org/s3fs/): another "s3fs", but this one is written in python
  * [s3fs-fuse](http://code.google.com/p/s3fs-fuse/): another S3 filesystem written in python
  * [riofs](https://github.com/skoobe/riofs): another S3 filesystem

Note also that Amazon's [Elastic Block Store](http://www.amazon.com/b?ie=UTF8&node=689343011&me=A36L942TSJ2AJA) allows you to mount Amazon-backed filesystems on EC2 instances over the network. This is a great way to get roughly the same effect that **s3backer** gives you, if you don't mind only having access to your data from an EC2 instance.