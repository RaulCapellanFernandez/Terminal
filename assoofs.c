#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
//#include <linux/uaccess.h>        /* copy_to_user          */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raul Capellan Fernandez");

//Inicializaciones
static struct kmem_cache *assoofs_inode_cache;
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data);
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
static int assoofs_create_aux(struct inode *dir, struct dentry *dentry, umode_t mode);
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
int assoofs_fill_super(struct super_block *sb, void *data, int silent);
int assoofs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * block);
int assoofs_inode_save(struct super_block *sb, struct assoofs_inode_info *inode_info);
struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search);
void assoofs_save_sb_info(struct super_block *vsb);
void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode);
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);


static struct file_system_type assoofs_type ={
    .owner   = THIS_MODULE,
    .name    = "assoofs",
    .mount   = assoofs_mount,
    .kill_sb = kill_litter_super,
};

static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
};

static const struct super_operations assoofs_sops = {
    .drop_inode = generic_delete_inode ,
};


const struct file_operations assoofs_file_operations =
{
	.read = assoofs_read,
	.write =assoofs_write,
};

const struct file_operations assoofs_dir_operations =
{
	.owner =THIS_MODULE,
	.iterate=assoofs_iterate,
};



//Escribir en un archivo
ssize_t assoofs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos){
	struct assoofs_inode_info *inode_info;
	struct buffer_head *bh;
	char *buffer;
	struct super_block *sb;

	sb=filp->f_path.dentry->d_inode->i_sb;
	inode_info=(struct assoofs_inode_info *) filp->f_path.dentry->d_inode->i_private;
	bh=sb_bread(filp->f_path.dentry->d_inode->i_sb, inode_info->data_block_number);

	if(!bh){
		printk(KERN_ERR "Reading the block number [%llu] failed.\n", inode_info->data_block_number);
		return 0;
	}

	buffer= (char *)bh->b_data;
	buffer+= *ppos;
	if(copy_from_user(buffer, buf, len)){
		brelse(bh);
		printk(KERN_ERR "Error copying from user to kernel\n");
		return -1;
	}

	*ppos+=len;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	inode_info->file_size=*ppos;
	assoofs_inode_save(sb, inode_info);
	return len;
}	

//Leer de un archivo
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos){
	struct assoofs_inode_info *inode_info =filp->f_path.dentry->d_inode->i_private;
	struct buffer_head *bh;
	char *buffer;
	size_t nbytes;

	//Comprobar el valor de ppos por si es final del fichero
	if (*ppos >= inode_info->file_size) 
		return 0;

	//Acceder al contenido del fichero
	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb, inode_info->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.\n",
		       inode_info->data_block_number);
		return 0;
	}

	//Copiar el en el buffer el contenido del fichero leido
	buffer = (char *)bh->b_data;
	nbytes = min((size_t) inode_info->file_size, len);

	if (copy_to_user(buf, buffer, nbytes)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents to the userspace buffer\n");
		return -EFAULT;
	}

	//Incrementar el valor de ppos y devolver el numero de bytes leidos
	*ppos += nbytes;
	brelse(bh);
	return nbytes;
}

//Mostrar el contenido de un directorio
static int assoofs_iterate(struct file *filp, struct dir_context *ctx){

	struct inode *inode;
	struct super_block *sb;
	struct assoofs_inode_info *inode_info;
	struct buffer_head *bh;
	struct assoofs_dir_record_entry *record;
	int i;

	//Accede al inodo correspondiente al argumento flip
	inode=filp->f_path.dentry->d_inode;
	sb=inode->i_sb;
	inode_info=(struct assoofs_inode_info *) inode->i_private;

	//Comprobar si el contexto del directorio esta ya creado
	if(ctx->pos)
		return 0;

	//Comprobar si el inodo corresponde con un directorio
	if(!S_ISDIR(inode_info->mode))
		return -1;

	//Accedemos al bloque donde se almacena el contenido del directorio
	bh=sb_bread(sb, inode_info->data_block_number);
	record=(struct assoofs_dir_record_entry *)bh->b_data;

	for(i=0;i<inode_info->dir_children_count;i++){
		dir_emit(ctx, record->filename, ASSOOFS_FILENAME_MAXLEN, record->inode_no, DT_UNKNOWN);
		ctx->pos+=sizeof(struct assoofs_dir_record_entry);
		record++;
	}

	brelse(bh);
	return 0;
}

//Puntero persistente de un inodo concreto
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search){
	uint64_t count=0;
	while(start->inode_no !=search->inode_no && count<((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count){
		count++;
		start++;
	}

	if(start->inode_no==search->inode_no)
		return start;
	else
		return NULL;
}

//Actulizar informacion persistente de un inodo
int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info){
	struct assoofs_inode_info *inode_pos;
	struct buffer_head *bh;

	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_pos=assoofs_search_inode_info(sb, (struct assoofs_inode_info *)bh->b_data, inode_info);

	//Actualiza el inodo
	if(inode_pos){
		memcpy(inode_pos, inode_info, sizeof(*inode_pos));
		printk(KERN_INFO "The inode has been updated");

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}else{

		printk(KERN_ERR "The new filesize could not be stored to the inode");
		return -EIO;
	}

	brelse(bh);
	return 0;
}

int assoofs_inode_save(struct super_block *sb, struct assoofs_inode_info *inode_info){
	struct assoofs_inode_info *inode_iterator;
	struct buffer_head *bh;

	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_iterator=assoofs_search_inode_info(sb, (struct assoofs_inode_info *)bh->b_data, inode_info);

	if(inode_iterator){
		memcpy(inode_iterator, inode_info, sizeof(*inode_iterator));
		printk(KERN_INFO "The inode has been updated");

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}else{

		printk(KERN_ERR "The new filesize could not be stored to the inode");
		return -EIO;
	}

	brelse(bh);
	return 0;
}

//Actualizar informacion del superbloque cuando hay un cambio
void assoofs_save_sb_info(struct super_block *vsb){
	struct buffer_head *bh;
	struct assoofs_super_block_info *sb=vsb->s_fs_info;

	//Leer y sobrescribir la informacion del superbloque
	bh=sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
	bh->b_data= (char *) sb;

	//Se sobrescribe en el disco
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

//Guarda la informacion de un inodo nuevo
void assoofs_add_inode_info(struct super_block *vsb, struct assoofs_inode_info *inode){
	//Obtiene el contador de inodos
	struct assoofs_super_block_info *sb =vsb->s_fs_info;
	struct buffer_head *bh;
	struct assoofs_inode_info *inode_info;

	//Leer de disco el bloque que contiene el alamacen de inodos
	bh=sb_bread(vsb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_info=(struct assoofs_inode_info *) bh->b_data;
	inode_info+=sb->inodes_count;
	memcpy(inode_info, inode, sizeof(struct assoofs_inode_info));

	//Marcar el bloque como sucio y sincronizar
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	//Actualizar el contador de inodos y guarda los cambios
	sb->inodes_count++;
	assoofs_save_sb_info(vsb);
}

//Obtener un bloque libre
int assoofs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * block){
	//Obtenemos la informacion anteriormente guardada
	struct assoofs_super_block_info *sb = vsb->s_fs_info;
	int i;
	int ret=0;

	//Recorremos el mapa en busca de un bloque libre
	for (i = ASSOOFS_RESERVED_INODES + 1; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
		if (sb->free_blocks & (1 << i)) 
			break;
	}

	if (unlikely(i == ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "No more free blocks available");
		ret = -ENOSPC;
	}

	//Actualizamos en el valor de free_block
	*block = i;
	sb->free_blocks &= ~(1 << i);
	assoofs_save_sb_info(vsb);

	return ret;
}

static int assoofs_create_aux(struct inode *dir, struct dentry *dentry, umode_t mode){
	struct inode *inode;
	struct assoofs_inode_info *inode_info;
	struct super_block *sb;
	struct assoofs_inode_info *parent_inode_info;
	struct buffer_head *bh;
	struct assoofs_dir_record_entry *dir_contents;
	uint64_t count;
	int ret;

	//Obtengo un puntero al superbloque desde dir
	sb = dir->i_sb;
	//Obtengo el numero de inodos de la informacion 
	count=((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count;
	inode=new_inode(sb);

	if (count < 0) {
		return ret;
	}
	//Numero maximo de objetos soportados
	if (unlikely(count >= ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		return -ENOSPC;
	}

	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		return -EINVAL;
	}
	if (!inode) {
		return -ENOMEM;
	}

	//Asigna los valores al nuevo inodo
	inode->i_sb = sb;
	inode->i_op = &assoofs_inode_ops;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

	inode->i_ino = (count + ASSOOFS_START_INO - ASSOOFS_RESERVED_INODES + 1);
	inode_info= kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
	inode_info->inode_no = inode->i_ino;
	inode_info->mode = mode;
	ret=assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);


	if(ret<0){
		printk(KERN_ERR "assoofs could not get a freeblock");
		return ret;
	}
	if (S_ISDIR(mode)) {
		inode_info->dir_children_count = 0;
		inode->i_fop = &assoofs_dir_operations;
	} else if (S_ISREG(mode)) {
		printk(KERN_INFO "New file creation request\n");
		inode_info->file_size = 0;
		inode->i_fop = &assoofs_file_operations;
	}
	inode->i_private=inode_info;
	assoofs_add_inode_info(sb, inode_info);

	//Modifica el contenido del directorio padre para el nuevo archivo o directorio
	parent_inode_info =(struct assoofs_inode_info *) dir->i_private;
	bh = sb_bread(sb, parent_inode_info->data_block_number);

	dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
	dir_contents += parent_inode_info->dir_children_count;
	dir_contents->inode_no = inode_info->inode_no;

	strcpy(dir_contents->filename, dentry->d_name.name);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	//Actualizar la informacion persistente del inodo padre
	parent_inode_info->dir_children_count++;
	ret = assoofs_save_inode_info(sb, parent_inode_info);
	
	if(ret)
		return ret;
	
	inode_init_owner(inode, dir, mode);
	d_add(dentry, inode);

	//LLega aqui si es incorrecto
	return 0;
}

//Crear nuevos inodos para archivos
static int assoofs_create(struct inode *dir , struct dentry *dentry , umode_t mode , bool excl){
	printk("Creando inodo\n");
	return assoofs_create_aux(dir, dentry,mode);
}

//Crear nuevos inodos para directorios
static int assoofs_mkdir(struct inode *dir , struct dentry *dentry , umode_t mode){
	printk("Creando directorio\n");
	return assoofs_create_aux(dir, dentry, S_IFDIR | mode);	
}

//Obtener puntero al inode  nuemro ino del superbloque
static struct inode *assoofs_get_inode(struct super_block *sb, int ino){
    struct assoofs_inode_info *inode_info;
    struct inode *i;

    i = new_inode(sb);
    inode_info=assoofs_get_inode_info(sb, ino);

    //Asignar valores a los campos del nuevo inodo
    i->i_ino = ino;
    i->i_op = &assoofs_inode_ops;
    i->i_sb=sb;

    if(S_ISDIR(inode_info->mode))
        i->i_fop=&assoofs_dir_operations;
    else if(S_ISREG(inode_info->mode))
        i->i_fop=&assoofs_file_operations;
    else
        printk(KERN_ERR "Unknown inode type. Neider a directory nor a file.");
    
    i->i_atime = current_time(i);
    i->i_mtime = current_time(i); 
    i->i_ctime = current_time(i);

    i->i_private=inode_info;

    //Devolvemos el inodo recien creado
    return i;
}

//Recorrer y mantener el arbol de inodos
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags){
    struct assoofs_inode_info *parent_info = parent_inode->i_private;
    struct super_block *sb = parent_inode->i_sb;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;

    //Acceder al bloque de disco con el contenido del padre
    bh = sb_bread(sb, parent_info->data_block_number);

    //Busca y si encuentra contruye su inodo
    record = (struct assoofs_dir_record_entry *)bh->b_data;
    for (i = 0; i < parent_info->dir_children_count; i++) {
        if (!strcmp(record->filename, child_dentry->d_name.name)) {
            struct inode *inode = assoofs_get_inode(sb, record->inode_no);
            inode_init_owner(inode, parent_inode, ((struct assoofs_inode_info *)inode->i_private)->mode);
            d_add(child_dentry, inode);
            brelse(bh);
            return NULL;
        }
        record++;
    }

    //Siempre tiene que devolver null
    brelse(bh);
    return NULL;
}

struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no){
    
    struct buffer_head *bh;
    struct assoofs_inode_info *inode_info=NULL;
    struct assoofs_inode_info *buffer=NULL;
    struct assoofs_super_block_info *afs_sb=sb->s_fs_info;
    int i;
    
    //Leer el bloque que contiene el almacen de inodos
    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    inode_info = (struct assoofs_inode_info *)bh->b_data;

    //Recorre el almacen de inodos en busca del inodo inode_no
    for (i = 0; i <afs_sb->inodes_count; i++){
        if ( inode_info->inode_no == inode_no){ 
            buffer=kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
            memcpy(buffer,inode_info,sizeof(*buffer));
            break;
        }
        inode_info++;
    }

    //libera recursos y devuelve la informacion
    brelse(bh);
    return buffer;
}

//Monta un nuevo dispositivo de bloques
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data){
    struct dentry *ret;
    ret=mount_bdev(fs_type,flags,dev_name, data, assoofs_fill_super);

    if(IS_ERR(ret))
        printk(KERN_ERR "Error mounting assoofs.");
    else
        printk(KERN_INFO "assoofs is sucessfully mounted on %s\n", dev_name);

    return ret;
}

//Iniciliza el superbloque
int assoofs_fill_super(struct super_block *sb, void *data, int silent){
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb;
    struct inode *root_inode;

    printk(KERN_DEBUG "-fill_super-\n");

    // Lee la informacion persistente del superbloque
    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    assoofs_sb = (struct assoofs_super_block_info *) bh->b_data;

    // Comprobar parametros del superbloque
    if (assoofs_sb->magic != ASSOOFS_MAGIC) {
        printk(KERN_ERR "Wrong magic number<: %llu\n",assoofs_sb->magic);
        return -1;
    }
    else
        printk(KERN_INFO "Correct magic number: %llu\n",assoofs_sb->magic);

    if (assoofs_sb->block_size != ASSOOFS_DEFAULT_BLOCK_SIZE) {
        printk(KERN_ERR "Wrong size block\n");
        return -1;
    }

    printk(KERN_INFO "File system assoofs in version %llu formated with a size block %llu\n", assoofs_sb->version, assoofs_sb->block_size);

    // Escribir informacion persistente leida
    sb->s_magic = ASSOOFS_MAGIC;
    sb->s_maxbytes = ASSOOFS_DEFAULT_BLOCK_SIZE;
    sb->s_fs_info = assoofs_sb;
    sb->s_op = &assoofs_sops;

    //Crear el inodo raiz
    root_inode = new_inode(sb);

    //Guardamos la informacion persistente
    root_inode->i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER;
    root_inode->i_sb = sb;
    root_inode->i_op = &assoofs_inode_ops;
    root_inode->i_fop = &assoofs_dir_operations;
    root_inode->i_atime = current_time(root_inode);
    root_inode->i_mtime = current_time(root_inode);
    root_inode->i_ctime = current_time(root_inode);
    root_inode->i_private = assoofs_get_inode_info(sb, ASSOOFS_ROOTDIR_INODE_NUMBER);

    inode_init_owner(root_inode, NULL, S_IFDIR);

    //marcamos inodo como raiz
    sb->s_root = d_make_root(root_inode);

    //libera recursos y devuelve la informacion
    brelse(bh);
    return 0;
}


//Inicia el nuevo sistema de ficheros
static int __init assoofs_init(void){
	int ret;
    assoofs_inode_cache=kmem_cache_create("assoofs_inode_cache", sizeof(struct assoofs_inode_info), 0, (SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD), NULL);
    
    if(!assoofs_inode_cache)
        return -ENOMEM;
    
    ret=register_filesystem(&assoofs_type);
    if(ret==0)
        printk(KERN_INFO "Sucessfully registered assoofs\n");
    else
        printk(KERN_ERR "Failed to register assoofs Error %d", ret);
    return ret;
}

//Cierra el sistema de ficheros creado
static void __exit assoofs_exit(void){
   int ret;
   ret=unregister_filesystem(&assoofs_type);
   kmem_cache_destroy(assoofs_inode_cache);
   if(ret==0)
       printk(KERN_INFO "Sucessfully unregistered assoofs\n");
   else
       printk(KERN_ERR "Failed to unregister assoofs, Error %d", ret);
}

module_init(assoofs_init);
module_exit(assoofs_exit);