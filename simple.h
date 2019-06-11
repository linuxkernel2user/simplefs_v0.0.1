#define SIMPLEFS_MAGIC 0x10032013
#define SIMPLEFS_DEFAULT_BLOCK_SIZE 4096
#define SIMPLEFS_FILENAME_MAXLEN 255
#define SIMPLEFS_START_INO 10
#define SIMPLEFS_MAX_FILESYSTEM_OBJECTS 64
#define SIMPLEFS_ROOTDIR_INODE_NUMBER 1
#define SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER 0
#define SIMPLEFS_INODESTORE_BLOCK_NUMBER 1
#define SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER 2


#define SIMPLEFS_RESERVED_INODES 3

struct simplefs_dir_record {
	char filename[SIMPLEFS_FILENAME_MAXLEN];
	uint64_t inode_no;
};

struct simplefs_inode {
	mode_t mode;
	unsigned int nlink;
	uint64_t inode_no;
	uint64_t data_block_number;
	
	union {
		uint64_t file_size;
		uint64_t dir_children_count;
	};
};

struct simplefs_super_block {
	uint64_t version;
	uint64_t magic;
	uint64_t block_size;

	uint64_t inodes_count;
	uint64_t inodes_bitmap;

	uint64_t free_blocks;

	char padding[SIMPLEFS_DEFAULT_BLOCK_SIZE - (6 * sizeof(uint64_t))];
};
