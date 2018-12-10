#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/tty.h>
#include <linux/mutex.h>
#include <linux/magic.h>
#include <linux/idr.h>
#include <linux/parser.h>
#include <linux/fsnotify.h>
#include <linux/seq_file.h>
#include <linux/pagemap.h>

static void ptree_create_files(struct super_block *sb,
			       struct dentry *root);

static int ptree_fill_super(struct super_block *sb, void *data, int silent);

static int __ptreefs_remove(struct dentry *dentry, struct dentry *parent)
{
	int ret = 0;

	if (simple_positive(dentry)) {
		dget(dentry);
		if (d_is_dir(dentry)) {
			printk("nlink: %ud\n", d_inode(dentry)->i_nlink);
			ret = simple_rmdir(d_inode(parent), dentry);
		}
		else
			simple_unlink(d_inode(parent), dentry);

		/* although it is not an empty folder, delete anyway.*/
		if (!ret) {
			d_invalidate(dentry);
			d_delete(dentry);
		}
		dput(dentry);
	}
	return ret;
}

void ptreefs_remove(struct dentry *dentry)
{
	struct dentry *parent;
	int ret;

	if (IS_ERR_OR_NULL(dentry)){
		printk("dentry is err or null\n");
		return;
	}

	parent = dentry->d_parent;
	if (!parent || d_really_is_negative(parent)){
		printk("dentry's parent is wrong\n");
		return;
	}


	mutex_lock(&d_inode(parent)->i_mutex);
	ret = __ptreefs_remove(dentry, parent);
	mutex_unlock(&d_inode(parent)->i_mutex);
}

static void ptreefs_remove_recursive(struct dentry *dentry)
{
	struct dentry *child, *parent;

	if (IS_ERR_OR_NULL(dentry))
		return;

	parent = dentry->d_parent;
	if (!parent || d_really_is_negative(parent))
		return;

	parent = dentry;
down:
	mutex_lock(&d_inode(parent)->i_mutex);
loop:
	spin_lock(&parent->d_lock);
	list_for_each_entry(child, &parent->d_subdirs, d_child) {
		printk("[%s] ", child->d_name.name);
	}
	printk("\n");
	list_for_each_entry(child, &parent->d_subdirs, d_child) {
	if (!simple_positive(child)) {
		printk("[%s] deleted succ\n", child->d_name.name);
		continue;
	}
	else
		printk("[%s] not deleted\n", child->d_name.name);

	/* perhaps simple_empty(child) makes more sense */
	if (!list_empty(&child->d_subdirs)) {
		spin_unlock(&parent->d_lock);
		mutex_unlock(&d_inode(parent)->i_mutex);
		parent = child;
		goto down;
	}

	spin_unlock(&parent->d_lock);

	if (!__ptreefs_remove(child, parent)) {
		printk("[%s] deleted\n", child->d_name.name);
	}

	/*
	 * The parent->d_lock protects agaist child from unlinking
	 * from d_subdirs. When releasing the parent->d_lock we can
	 * no longer trust that the next pointer is valid.
	 * Restart the loop. We'll skip this one with the
	 * simple_positive() check.
	 */
	goto loop;
	}
	spin_unlock(&parent->d_lock);

	mutex_unlock(&d_inode(parent)->i_mutex);
	child = parent;
	parent = parent->d_parent;
	mutex_lock(&d_inode(parent)->i_mutex);

	if (child != dentry)
		/* go up */
		goto loop;

	printk("Root [%s] visited\n", child->d_name.name);
	if (!__ptreefs_remove(child, parent)) {
		printk("[%s] deleted\n", child->d_name.name);
	}
	mutex_unlock(&d_inode(parent)->i_mutex);
}

static void repslash(char* oristr)
{
        char *p = oristr;
        while(*p != '\0') {
                if (*p == '/') {
                        *p = '-';
                }
                p++;
        }
}

static ssize_t ptreefs_read_file(struct file *file, char __user *buf,
size_t count, loff_t *ppos)
{
	return 0;
};

int ptreefs_root_dir_open(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct dentry *dentry, *child;

	dentry = file_dentry(file);

	if (!list_empty(&dentry->d_subdirs)) {
		child = list_first_entry(&dentry->d_subdirs, struct dentry, d_child);
		ptreefs_remove_recursive(child);
	}

	ptree_create_files(sb, sb->s_root);

	return dcache_dir_open(inode, file);
}

static ssize_t ptreefs_write_file(struct file *file, const char __user *buf,
size_t count, loff_t *ppos)
{
	return count;
};

const struct file_operations ptreefs_file_operations = {
	.open		= simple_open,
	.read		= ptreefs_read_file,
	.write		= ptreefs_write_file,
};

const struct file_operations ptreefs_root_dir_operations = {
	.open		= ptreefs_root_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate	= dcache_readdir,
	.fsync		= noop_fsync,
};

struct inode *ptree_make_inode(struct super_block *sb,
			       int mode)
{
	struct inode *inode;
	inode = new_inode(sb);

	/* if (!inode) */
	/* 	return -ENOMEM; */

	inode->i_ino = get_next_ino();
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_mode = mode;
	inode->i_blkbits = PAGE_CACHE_SIZE;
	inode->i_blocks = 0;

	return inode;
};

static struct dentry *ptree_mount(struct file_system_type *fs_type,
                int flags, const char *dev_name,
                void *data)
{
        return mount_single(fs_type, flags, data, ptree_fill_super);
}

static struct file_system_type ptree_fs_type = {
        .owner          = THIS_MODULE,
        .name		= "ptreefs",
        .mount		= ptree_mount,
        .kill_sb	= kill_litter_super
};

struct dentry *ptree_create_dir(struct super_block *sb,
				 struct dentry *parent, const char *name)
{
	struct dentry *dentry;
	struct inode *inode;
	struct qstr qname;

	qname.name	= name;
	qname.len	= strlen (name);
	qname.hash	= full_name_hash(name, qname.len);

	dentry = d_alloc(parent, &qname);
	/* if (! dentry) */
	/* 	goto out; */

	inode = ptree_make_inode(sb, S_IFDIR | 0777);
	/* if (! inode) */
	/* 	goto out_dput; */

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;

	d_add(dentry, inode);
	return dentry;

	/* out_dput: */
	/* 	dput(dentry); */
	/* out: */
	/* 	return 0; */
};

struct dentry *ptree_create_file(struct super_block *sb,
		struct dentry *dir, const char *name)
{
	struct inode *inode;
	struct dentry *dentry;
	struct qstr qname;

	qname.name	= name;
	qname.len	= strlen(name);
	qname.hash	= full_name_hash(name, qname.len);

	inode = ptree_make_inode(sb, S_IFREG | 0777);
	/* if (!inode) */
	/* 	return -ENOMEM; */
	inode->i_fop = &ptreefs_file_operations;

	dentry = d_alloc(dir, &qname);
	/* if (!dentry) */
	/* 	return -ENOMEM; */

	d_add(dentry, inode);
	return dentry;
};

static void ptree_create_files(struct super_block *sb,
		struct dentry *root)
{
	char *name = kmalloc(TASK_COMM_LEN + 5, GFP_KERNEL);
	struct dentry *subdir = root;
	struct task_struct *init = &init_task;
	struct task_struct *p = init;
	long pid;
	char s_pid[6];

	bool going_up = false;

	read_lock(&tasklist_lock);
	while (!going_up || likely(p != init)) {
		if (!going_up) {
			/* Get PID for task */
			pid = (long)task_pid_nr(p);
			sprintf(s_pid, "%ld", pid);

			/* Create a directory for task */
			subdir = ptree_create_dir(sb, subdir, s_pid);

			get_task_comm(name, p);
                        repslash(name);
			strcat(name, ".name");
			ptree_create_file(sb, subdir, name);

		}
		if (!going_up && !list_empty(&p->children)) {
			p = list_last_entry(&p->children, struct task_struct,
					sibling);
		} else if (p->sibling.prev != &p->real_parent->children) {
			p = list_last_entry(&p->sibling, struct task_struct,
					sibling);
			subdir = subdir->d_parent;
			going_up = false;
		} else {
			p = p->real_parent;
			subdir = subdir->d_parent;
			going_up = true;
		}
	}
	read_unlock(&tasklist_lock);
	kfree(name);
};

static const struct super_operations ptreefs_s_ops = {
        .statfs         = simple_statfs,
        .drop_inode     = generic_delete_inode,
};

static int ptree_fill_super(struct super_block *sb, void *data, int silent)
{
        int err;
        static struct tree_descr ptree_files[] = {{""}};
        struct inode *inode;

        err  =  simple_fill_super(sb, PTREEFS_MAGIC, ptree_files);
        if (err)
                goto fail;
        sb->s_op = &ptreefs_s_ops;

        inode = new_inode(sb);
        if (!inode)
                goto fail;

        inode->i_ino = 1;
        inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
        inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO | S_IWUSR;
        inode->i_op = &simple_dir_inode_operations;
        //inode->i_fop = &ptreefs_file_ops;
	inode->i_fop = &ptreefs_root_dir_operations;

        sb->s_root = d_make_root(inode);
        if (!sb->s_root)
                goto fail;
	ptree_create_files(sb, sb->s_root);

	return 0;

fail:
	pr_err("get root dentry failed\n");
        return -ENOMEM;
}

static int __init ptreefs_init(void)
{
        return register_filesystem(&ptree_fs_type);
}
module_init(ptreefs_init);
