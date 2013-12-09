/*
 * Copyright 2013 Frantisek Hrbata <fhrbata@pluginfs.org>
 * 
 * This file is part of PluginFS.
 *
 * PluginFS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PluginFS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PluginFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "plgfs.h"

static struct dentry *plgfs_dir_iop_lookup(struct inode *i, struct dentry *d,
		unsigned int flags)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct dentry *dph; /* dentry parent hidden */
	struct dentry *dh; /* dentry hidden */
	struct dentry *rv;

	sbi = plgfs_sbi(i->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return ERR_CAST(cont);

	cont->op_id = PLGFS_DIR_IOP_LOOKUP;
	cont->op_args.i_lookup.dir = i;
	cont->op_args.i_lookup.dentry = d;
	cont->op_args.i_lookup.flags = flags;

	if(!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	i = cont->op_args.i_lookup.dir;
	d = cont->op_args.i_lookup.dentry;
	flags = cont->op_args.i_lookup.flags;

	dph = plgfs_dh(d->d_parent);

	mutex_lock(&dph->d_inode->i_mutex);
	dh = lookup_one_len(d->d_name.name, dph, d->d_name.len);
	mutex_unlock(&dph->d_inode->i_mutex);

	if (IS_ERR(dh)) {
		cont->op_rv.rv_dentry = dh;
		goto postcalls;
	}

	d->d_fsdata = plgfs_alloc_di(d, dh);
	if (IS_ERR(d->d_fsdata)) {
		cont->op_rv.rv_dentry = ERR_CAST(d->d_fsdata);
		dput(dh);
		goto postcalls;
	}

	if (!dh->d_inode) {
		d_add(d, NULL);
		goto postcalls;
	}

	i = plgfs_iget(i->i_sb, (unsigned long)dh->d_inode);
	/* dput of our dentry will also free the hidden one */
	if (IS_ERR(i)) {
		cont->op_rv.rv_dentry = ERR_CAST(i);
		goto postcalls;
	}

	d_add(d, i);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_dentry;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_dir_iop_create(struct inode *ip, struct dentry *d,
		umode_t mode, bool excl)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct inode *iph; /* dentry parent hidden */
	struct dentry *dh; /* dentry hidden */
	struct inode *i;
	int rv;

	sbi = plgfs_sbi(ip->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_DIR_IOP_CREATE;
	cont->op_args.i_create.dir = ip;
	cont->op_args.i_create.dentry = d;
	cont->op_args.i_create.mode = mode;
	cont->op_args.i_create.excl = excl;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	ip = cont->op_args.i_create.dir;
	d = cont->op_args.i_create.dentry;
	mode = cont->op_args.i_create.mode;
	excl = cont->op_args.i_create.excl;

	iph = plgfs_ih(ip);
	dh = plgfs_dh(d);

	mutex_lock_nested(&iph->i_mutex, I_MUTEX_PARENT);
	cont->op_rv.rv_int = vfs_create(iph, dh, mode, excl);
	mutex_unlock(&iph->i_mutex);
	if (cont->op_rv.rv_int)
		goto postcalls;

	i = plgfs_iget(ip->i_sb, (unsigned long)dh->d_inode);
	if (IS_ERR(i)) {
		mutex_lock_nested(&iph->i_mutex, I_MUTEX_PARENT);
		rv = vfs_unlink(iph, dh);
		mutex_unlock(&iph->i_mutex);
		if (rv)
			pr_err("pluginfs: create: unlink failed: %d\n", rv);

		cont->op_rv.rv_int = PTR_ERR(i);
		goto postcalls;
	}

	fsstack_copy_attr_times(ip, iph);
	d_instantiate(d, i);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_reg_iop_setattr(struct dentry *d, struct iattr *ia)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct dentry *dh;
	struct file *f;
	int rv;

	sbi = plgfs_sbi(d->d_inode->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_REG_IOP_SETATTR;
	cont->op_args.i_setattr.dentry = d;
	cont->op_args.i_setattr.iattr = ia;

	f = ia->ia_file;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	if (ia->ia_valid & ATTR_FILE)
		ia->ia_file = plgfs_fh(f);

	d = cont->op_args.i_setattr.dentry;
	ia = cont->op_args.i_setattr.iattr;
	dh = plgfs_dh(d);

	mutex_lock(&dh->d_inode->i_mutex);
	cont->op_rv.rv_int = notify_change(dh, ia);
	mutex_unlock(&dh->d_inode->i_mutex);

	fsstack_copy_attr_all(d->d_inode, dh->d_inode);
	fsstack_copy_inode_size(d->d_inode, dh->d_inode);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	ia->ia_file = f;

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_dir_iop_unlink(struct inode *i, struct dentry *d)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct inode *ih;
	int rv;

	sbi = plgfs_sbi(d->d_inode->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_DIR_IOP_UNLINK;
	cont->op_args.i_unlink.dir = i;
	cont->op_args.i_unlink.dentry = d;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	i = cont->op_args.i_unlink.dir;
	d = cont->op_args.i_unlink.dentry;
	ih = plgfs_ih(i);

	mutex_lock_nested(&ih->i_mutex, I_MUTEX_PARENT);
	cont->op_rv.rv_int = vfs_unlink(ih, plgfs_dh(d));
	mutex_unlock(&ih->i_mutex);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_dir_iop_mkdir(struct inode *ip, struct dentry *d, umode_t m)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct inode *iph;
	struct inode *i;
	struct dentry *dh;
	int rv;

	sbi = plgfs_sbi(ip->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_DIR_IOP_MKDIR;
	cont->op_args.i_mkdir.dir = ip;
	cont->op_args.i_mkdir.dentry = d;
	cont->op_args.i_mkdir.mode = m;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	ip = cont->op_args.i_mkdir.dir;
	d = cont->op_args.i_mkdir.dentry;
	m = cont->op_args.i_mkdir.mode;
	iph = plgfs_ih(ip);
	dh = plgfs_dh(d);

	mutex_lock_nested(&iph->i_mutex, I_MUTEX_PARENT);
	cont->op_rv.rv_int = vfs_mkdir(iph, dh, m);
	mutex_unlock(&iph->i_mutex);

	if (cont->op_rv.rv_int)
		goto postcalls;

	i = plgfs_iget(ip->i_sb, (unsigned long)dh->d_inode);
	if (IS_ERR(i)) {
		mutex_lock_nested(&iph->i_mutex, I_MUTEX_PARENT);
		rv = vfs_unlink(iph, dh);
		mutex_unlock(&iph->i_mutex);
		if (rv)
			pr_err("pluginfs: mkdir: unlink failed: %d\n", rv);

		cont->op_rv.rv_int = PTR_ERR(i);
		goto postcalls;
	}

	d_instantiate(d, i);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_dir_iop_rmdir(struct inode *ip, struct dentry *d)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct inode *iph;
	int rv;

	sbi = plgfs_sbi(d->d_inode->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_DIR_IOP_RMDIR;
	cont->op_args.i_mkdir.dir = ip;
	cont->op_args.i_mkdir.dentry = d;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	ip = cont->op_args.i_mkdir.dir;
	d = cont->op_args.i_mkdir.dentry;
	iph = plgfs_ih(ip);

	mutex_lock_nested(&iph->i_mutex, I_MUTEX_PARENT);
	cont->op_rv.rv_int = vfs_rmdir(plgfs_ih(ip), plgfs_dh(d));
	mutex_unlock(&iph->i_mutex);

postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static int plgfs_dir_iop_rename(struct inode *oi, struct dentry *od,
		struct inode *ni, struct dentry *nd)
{
	struct plgfs_context *cont;
	struct plgfs_sb_info *sbi;
	struct inode *oih;
	struct dentry *odh;
	struct inode *nih;
	struct dentry *ndh;
	struct dentry *trap;
	int rv;

	sbi = plgfs_sbi(oi->i_sb);
	cont = plgfs_alloc_context(sbi);
	if (IS_ERR(cont))
		return PTR_ERR(cont);

	cont->op_id = PLGFS_DIR_IOP_RENAME;
	cont->op_args.i_rename.old_dir = oi;
	cont->op_args.i_rename.old_dentry = od;
	cont->op_args.i_rename.new_dir = ni;
	cont->op_args.i_rename.new_dentry = nd;

	if (!plgfs_precall_plgs(cont, sbi))
		goto postcalls;

	oi = cont->op_args.i_rename.old_dir;
	od = cont->op_args.i_rename.old_dentry;
	ni = cont->op_args.i_rename.new_dir;
	nd = cont->op_args.i_rename.new_dentry;

	oih = plgfs_ih(oi);
	odh = plgfs_dh(od);
	nih = plgfs_ih(ni);
	ndh = plgfs_dh(nd);

	trap = lock_rename(ndh->d_parent, odh->d_parent);

	if (trap == odh) {
		cont->op_rv.rv_int = -EINVAL;
		goto trapped;
	}

	if (trap == ndh) {
		cont->op_rv.rv_int = -ENOTEMPTY;
		goto trapped;
	}

	cont->op_rv.rv_int = vfs_rename(oih, odh, nih, ndh);

trapped:
	unlock_rename(ndh->d_parent, odh->d_parent); 
postcalls:
	plgfs_postcall_plgs(cont, sbi);

	rv = cont->op_rv.rv_int;

	plgfs_free_context(sbi, cont);

	return rv;
}

static const struct inode_operations plgfs_reg_iops= {
	.setattr = plgfs_reg_iop_setattr
};

static const struct inode_operations plgfs_dir_iops= {
	.lookup = plgfs_dir_iop_lookup,
	.create = plgfs_dir_iop_create,
	.unlink = plgfs_dir_iop_unlink,
	.mkdir = plgfs_dir_iop_mkdir,
	.rmdir = plgfs_dir_iop_rmdir,
	.setattr = plgfs_reg_iop_setattr,
	.rename = plgfs_dir_iop_rename
};

struct inode *plgfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *i;
	struct inode *ih;
	struct plgfs_inode_info *ii;

	ih = (struct inode *)ino;

	if (!igrab(ih))
		return ERR_PTR(-ESTALE);

	i = iget_locked(sb, ino);
	if (!i) {
		iput(ih);
		return ERR_PTR(-ENOMEM);
	}

	if (!(i->i_state & I_NEW)) {
		iput(ih);
		return i;
	}

	ii = plgfs_alloc_ii(plgfs_sbi(sb));
	if (IS_ERR(ii)) {
		iput(ih);
		return ERR_CAST(ii);
	}

	i->i_private = ii;
	ii->inode_hidden = ih;
	fsstack_copy_attr_all(i, ih);
	fsstack_copy_inode_size(i, ih);

	if (S_ISREG(i->i_mode)) {
		i->i_op = &plgfs_reg_iops;
		i->i_fop = &plgfs_reg_fops;
	} else if (S_ISDIR(i->i_mode)) {
		i->i_op = &plgfs_dir_iops;
		i->i_fop = &plgfs_dir_fops;
	}


	unlock_new_inode(i);

	return i;
}

struct plgfs_inode_info *plgfs_alloc_ii(struct plgfs_sb_info *sbi)
{	
	struct plgfs_inode_info *ii;

	ii = kmem_cache_zalloc(sbi->cache->ii_cache, GFP_KERNEL);
	if (!ii)
		return ERR_PTR(-ENOMEM);

	mutex_init(&ii->file_hidden_mutex);

	return ii;
}
