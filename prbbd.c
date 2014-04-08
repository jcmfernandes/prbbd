/**
 * Copyright (c) 2013 João Fernandes <mail youknowwhat joaofernandes putadothere eu>
 *
 * Parts derived from drivers/devices/mtd/phram.c, copyright of their
 * respective owners.
 *
 * Usage:
 *
 * one commend line parameter per device, each in the form:
 *   prbbd=<name>,<start>,<len>
 * <name> may be up to 63 characters.
 * <start> and <len> can be octal, decimal or hexadecimal.  If followed
 * by "K", "M" or "G", the numbers will be interpreted as kilo, mega or
 * gigabytes.
 *
 * Example:
 *	prbbd=swap,64M,128M prbbd=test,900M,1M
 */

#define pr_fmt(fmt) "prbbd: " fmt

#include <linux/module.h>

#include <linux/blkdev.h>
#include <linux/hdreg.h>


#define REQUEST_MODE	1
#define RM_SIMPLE	0
#define RM_NOQUEUE	1


#define KERNEL_SECTOR_SIZE_SHIFT	9
#define KERNEL_SECTOR_SIZE		(1 << KERNEL_SECTOR_SIZE_SHIFT)
#define SECTOR_SIZE_SHIFT		9
#define SECTOR_SIZE			(1 << SECTOR_SIZE_SHIFT)

#define PRBBD_MINORS			16


struct prbbd_dev {
	unsigned long size; /* Device size in sectors */
	void __iomem *data; /* Pointer to memory */
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;

	struct list_head list;
};

struct prbbd_param {
	char param[64 + 20 + 20 + 1];

	struct list_head list;
};

static LIST_HEAD(prbbd_param_list);
static LIST_HEAD(prbbd_list);
static int prbbd_major;


static void prbbd_transfer(struct prbbd_dev *dev, sector_t sector,
			   sector_t nsect, char *buffer, int write)
{
	unsigned long offset = sector << KERNEL_SECTOR_SIZE_SHIFT;
	unsigned long nbytes = nsect << KERNEL_SECTOR_SIZE_SHIFT;

	if (offset + nbytes > dev->size << SECTOR_SIZE_SHIFT) {
		printk(KERN_NOTICE "Beyond-end access (offset: %ld / bytes: %ld)\n",
		       offset, nbytes);
		return;
	}

	if (write)
		memcpy_toio(dev->data + offset, buffer, nbytes);
	else
		memcpy_fromio(buffer, dev->data + offset, nbytes);

	return;
}

static void prbbd_request(struct request_queue *q)
{
	struct request *req;
	struct prbbd_dev *prbbd;

	req = blk_fetch_request(q);
	while (req != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}

		prbbd = q->queuedata;
		prbbd_transfer(prbbd, blk_rq_pos(req), blk_rq_cur_sectors(req),
			       req->buffer, rq_data_dir(req));

		if (! __blk_end_request_cur(req, 0))
			req = blk_fetch_request(q);
	}

	return;
}

static int prbbd_xfer_bio(struct prbbd_dev *prbbd, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
                sector_t nsect = bio_cur_bytes(bio) >> KERNEL_SECTOR_SIZE_SHIFT;
		prbbd_transfer(prbbd, sector, nsect,
			       buffer, bio_data_dir(bio) == WRITE);
		sector += nsect;
		__bio_kunmap_atomic(bio, KM_USER0);
	}

	return 0;
}

static int prbbd_make_request(struct request_queue *q, struct bio *bio)
{
	struct prbbd_dev *prbbd;
	int status;

	prbbd = q->queuedata;
	status = prbbd_xfer_bio(prbbd, bio);
	bio_endio(bio, status);

        return 0;
}

static int prbbd_getgeo(struct block_device *bdev, struct hd_geometry *geo) {
	unsigned long size;
	struct prbbd_dev *prbbd;

	prbbd = bdev->bd_disk->private_data;
	size = prbbd->size * (SECTOR_SIZE / KERNEL_SECTOR_SIZE);

	/* We have no real geometry, of course, so make something up. */
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;

	return 0;
}

static struct block_device_operations prbbd_ops = {
	.owner		= THIS_MODULE,
	.getgeo		= prbbd_getgeo
};

static void unregister_devices(void)
{
	struct prbbd_dev *this, *safe;

	list_for_each_entry_safe(this, safe, &prbbd_list, list) {
		if (this->gd) {
			del_gendisk(this->gd);
			put_disk(this->gd);
		}

		if (this->queue)
			blk_cleanup_queue(this->queue);

		if (this->data)
			iounmap(this->data);

		kfree(this);
	}
}

static int register_device(char *name, resource_size_t start /* in bytes */,
			   unsigned long len /* in bytes */)
{
	static int i = 0;
	struct prbbd_dev *new;
	int ret = -ENOMEM;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (! new)
		goto out0;

	ret = -EIO;

	new->size = len / SECTOR_SIZE;
	new->data = ioremap(start, len);
	if (! new->data) {
		pr_err("ioremap failed\n");
		goto out1;
	}

	spin_lock_init(&new->lock);
        switch (REQUEST_MODE) {
        case RM_SIMPLE:
                new->queue = blk_init_queue(prbbd_request, &new->lock);
                if (! new->queue) {
                        pr_err("blk_init_queue failed\n");
                        goto out2;
                }
                break;
        case RM_NOQUEUE:
                new->queue = blk_alloc_queue(GFP_KERNEL);
                if (! new->queue) {
                        pr_err("blk_alloc_queue failed\n");
                        goto out2;
                }
                blk_queue_make_request(new->queue, prbbd_make_request);
                break;
        }
	blk_queue_logical_block_size(new->queue, SECTOR_SIZE);
	new->queue->queuedata = new;

	new->gd = alloc_disk(PRBBD_MINORS);
	if (! new->gd) {
		pr_err("alloc_disk failed\n");
		goto out2;
	}
	new->gd->major = prbbd_major;
	new->gd->first_minor = PRBBD_MINORS * i;
	new->gd->fops = &prbbd_ops;
	new->gd->queue = new->queue;
	new->gd->private_data = new;
	strcpy(new->gd->disk_name, name);
	set_capacity(new->gd, new->size * (SECTOR_SIZE / KERNEL_SECTOR_SIZE));
	add_disk(new->gd);

	list_add_tail(&new->list, &prbbd_list);
	i++;

	return 0;

out2:
	iounmap(new->data);
out1:
	kfree(new);
out0:
	return ret;
}

static int parse_name(char **pname, const char *token)
{
	size_t len;
	char *name;

	len = strlen(token) + 1;
	if (len > 64)
		return -ENOSPC;

	name = kmalloc(len, GFP_KERNEL);
	if (! name)
		return -ENOMEM;

	strcpy(name, token);

	*pname = name;
	return 0;
}

static int parse_num(unsigned long long *num, const char *token)
{
	unsigned long long result;

	result = memparse(token, NULL);
	if (! result)
		return -EINVAL;

	*num = result;
	return 0;
}

static inline void kill_final_newline(char *str)
{
	char *newline = strrchr(str, '\n');
	if (newline && !newline[1])
		*newline = 0;
}


#define parse_err(fmt, args...) do {		\
		pr_err(fmt , ## args);		\
		return 1;			\
	} while (0)

static int __init prbbd_setup(const char *val)
{
	char buf[64 + 20 + 20 + 1], *str = buf;
	char *token[3];
	char *name;
	unsigned long long start;
	unsigned long long len;

	int i, ret;

	if (strnlen(val, sizeof(buf)) >= sizeof(buf))
		parse_err("parameter too long\n");

	strcpy(str, val);
	kill_final_newline(str);

	for (i = 0; i < 3; i++)
		token[i] = strsep(&str, ",");

	if (str)
		parse_err("too many arguments\n");

	if (! token[2])
		parse_err("not enough arguments\n");

	ret = parse_name(&name, token[0]);
	if (ret)
		return ret;

	ret = parse_num(&start, token[1]);
	if (ret) {
		kfree(name);
		parse_err("illegal start address\n");
	}

	ret = parse_num(&len, token[2]);
	if (ret) {
		kfree(name);
		parse_err("illegal device length\n");
	}

	ret = register_device(name, (resource_size_t)start, (unsigned long)len);
	if (! ret)
		pr_info("%s device: %#llx at %#llx\n", name, len, start);

	return ret;
}

static int __init prbbd_param_call(const char *val, struct kernel_param *kp)
{
	/*
	 * This function is always called before 'init_prbbd()', whether
	 * built-in or module.
	 */
	struct prbbd_param *new;
	int ret = -ENOMEM;

	printk(KERN_NOTICE "prbbd_param_call got: %s\n", val);

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (! new)
		goto out0;

	ret = -ENOSPC;
	if (strlen(val) >= sizeof(new->param) - 1)
		goto out1;

	strcpy(new->param, val);
	list_add_tail(&new->list, &prbbd_param_list);

	return 0;
out1:
	kfree(new);
out0:
	return ret;
}

module_param_call(prbbd, prbbd_param_call, NULL, NULL, 000);
MODULE_PARM_DESC(prbbd, "Memory region to map. \"prbbd=<name>,<start>,<length>\"");


static int __init init_prbbd(void)
{
	struct prbbd_param *this, *safe;
	int ret = -EIO;

	prbbd_major = register_blkdev(prbbd_major, "prbbd");
	if (prbbd_major <= 0) {
		pr_err("register_blkdev failed\n");
		goto out0;
	}

	list_for_each_entry_safe(this, safe, &prbbd_param_list, list) {
		list_del(&this->list);

		ret = prbbd_setup(this->param);
		kfree(this);
		if (ret) {
			pr_err("prbbd_setup failed\n");
			goto out0;
		}
	}

	return 0;
out0:
	list_for_each_entry_safe(this, safe, &prbbd_param_list, list) {
		kfree(this);
	}
	return ret;
}

static void __exit cleanup_prbbd(void)
{
	unregister_devices();
	unregister_blkdev(prbbd_major, "prbbd");
}

module_init(init_prbbd);
module_exit(cleanup_prbbd);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("João Fernandes <mail youknowwhat joaofernandes putadothere eu>");
MODULE_DESCRIPTION("Persistent RAM Backed Block Device Driver");
