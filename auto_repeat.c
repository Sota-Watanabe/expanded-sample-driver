//auto_repeat
/**
 * Sample driver for tact switch
 * File name: auto repeat
 * Target board: Armadillo 440
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/timer.h>
#include <linux/init.h>

#define N_TACTSW	1	// number of minor devices
#define MSGLEN		256	// buffer length
#define INTERVAL (70) 
#define INTERVAL_SHORT (10)
#define SW_A 1
#define SW_B 2
#define SW_C 4
int ch = '0',pre_ch = '0';//グローバル変数　
static int tactsw_buttons[] = {	// board dependent parameters
  GPIO(3, 30),	// SW1
#if defined(CONFIG_MACH_ARMADILLO440)
  GPIO(2, 20),	// LCD_SW1
  GPIO(2, 29),	// LCD_SW2
  GPIO(2, 30),	// LCD_SW3
#if defined(CONFIG_ARMADILLO400_GPIO_A_B_KEY)
  GPIO(1, 0),	// SW2
  GPIO(1, 1),	// SW3
#endif /* CONFIG_ARMADILLO400_GPIO_A_B_KEY */
#endif /* CONFIG_MACH_ARMADILLO440 */
};

struct timer_list my_timer;//カーネルタイマー宣言

//static struct pid *my_pid;
// character device
static struct cdev tactsw_dev;
// Info for the driver
static struct {
  int major;			// major number
  int nbuttons;			// number of tact switchs
  int *buttons;			// hardware parameters
  int used;			// true when used by a process,
  				// this flag inhibits open twice.
  int mlen;			// buffer filll count
  char msg[MSGLEN];		// buffer
  wait_queue_head_t wq;		// queue of procs waiting new input
  spinlock_t slock;		// for spin lock
  }tactsw_info;
  
static void my_timer_handler(unsigned long arg){//タイムアウト関数
  int mlen;
  unsigned long irqflags;
 if(pre_ch != ch){
   mod_timer(&my_timer,jiffies+INTERVAL);//mod_timer()にはerrorが存在しない
 }else{
   mod_timer(&my_timer,jiffies+INTERVAL_SHORT);//mod_timer()にはerrorが存在しない
   if(ch != '0'){
     spin_lock_irqsave(&(tactsw_info.slock), irqflags);
     mlen = tactsw_info.mlen;
     if (mlen < MSGLEN) {
       tactsw_info.msg[mlen] = ch;
       tactsw_info.mlen = mlen+1;
       wake_up_interruptible(&(tactsw_info.wq));
     }
     spin_unlock_irqrestore(&(tactsw_info.slock), irqflags);
   } 
 }
 pre_ch = ch;
}

static int tactsw_open(struct inode *inode, struct file *filp)
{
  init_timer(&my_timer);//カーネルタイマー初期化!
  my_timer.expires = jiffies + INTERVAL;//ミリ秒後に起きる
  my_timer.function = my_timer_handler;//タイムアウト関数指定
  add_timer(&my_timer);
  int i;
  unsigned long irqflags;
  int retval = -EBUSY;
  spin_lock_irqsave(&(tactsw_info.slock), irqflags);
  if (tactsw_info.used == 0) {
    tactsw_info.used = 1;
    retval = 0;
  }
  spin_unlock_irqrestore(&(tactsw_info.slock), irqflags);   
  return retval;
}

static int tactsw_read(struct file *filp, char *buff,
		       size_t count, loff_t *pos)
{
  char *p1, *p2;
  size_t read_size;
  int i, ret;
  unsigned long irqflags;
  if (count <= 0) return -EFAULT;
  ret = wait_event_interruptible(tactsw_info.wq, (tactsw_info.mlen != 0) );
  if (ret != 0) return -EINTR;		// interrupted
  read_size = tactsw_info.mlen;		// atomic, so needless to spin lock
  if (count < read_size) read_size = count;
  if (copy_to_user(buff, tactsw_info.msg, read_size)) {
    printk("tactsw: copy_to_user error\n");
    // spin_unlock_irqrestore()
    return -EFAULT;
  } 
  // Ring buffer is better.  But we prefer simplicity.
  p1 = tactsw_info.msg;
  p2 = p1+read_size;
  
  spin_lock_irqsave(&(tactsw_info.slock), irqflags);
  // This subtraction is safe, since there is a single reader.
  tactsw_info.mlen -= read_size;//通常：ここで0になる　特殊：0にならない
  for (i=tactsw_info.mlen; i>0; i--){
    *p1++=*p2++;
  }
  spin_unlock_irqrestore(&(tactsw_info.slock), irqflags);
  return read_size;//←いつも1
}

int get_num(){
  int num = 0;
  if(!gpio_get_value(tactsw_info.buttons[1]))num += SW_A;
  if(!gpio_get_value(tactsw_info.buttons[2]))num += SW_B;
  if(!gpio_get_value(tactsw_info.buttons[3]))num += SW_C;
  return num + 48;//ASCIIコード表により変換
}

int tactsw_ioctl(struct inode *inode, struct file *filp,
		 unsigned int cmd, unsigned long arg)
{
  static struct pid *my_pid;
  int retval=0,i,errno,error;
  switch(cmd){
  case 1://fetch current status
    for (i = 0; i < tactsw_info.nbuttons; i++) {
      int gpio = tactsw_info.buttons[i],num;   
      if(gpio_get_value(gpio)){
		num = get_num();
		if(!num)return;//num が0のときは何も返さない
		return num - 48;//ASCIIコード表により変換
      }
    }
    break;
  case 2:// set signal
    my_pid = get_pid(task_pid(current));
    break;
  case 3:// clear signal
    put_pid(my_pid); 
    break;
  case 4:// send signal
    error  = kill_pid(my_pid,SIGUSR1,1);
    if(!error){
      printk("%d\n",errno);
      retval = -EFAULT;// other code may be better
    }
    break;
  default:
    retval = -EFAULT;	// other code may be better
  }
  return retval;
}

static int tactsw_release(struct inode *inode, struct file *filp)
{
  int error;
  del_timer(&my_timer);
  tactsw_info.used = 0;
  return 0;
}

static irqreturn_t tactsw_intr(int irq, void *dev_id)
{
  int i;
  for (i = 0; i < tactsw_info.nbuttons; i++) {
    int gpio = tactsw_info.buttons[i];   
    if (irq == gpio_to_irq(gpio)) {
      int mlen;
      unsigned long irqflags;
      ch = get_num();//関数get_num()よりchを指定する 
      spin_lock_irqsave(&(tactsw_info.slock), irqflags);
      mlen = tactsw_info.mlen;
      if (mlen < MSGLEN) {
	tactsw_info.msg[mlen] = ch;
	tactsw_info.mlen = mlen + 1;
	wake_up_interruptible(&(tactsw_info.wq));
      }
      spin_unlock_irqrestore(&(tactsw_info.slock), irqflags);
      return IRQ_HANDLED;
    }
  }
  return IRQ_NONE;
}
// .XXX = という書き方は，gccという特殊なCに独自の機能で，
// XXX という構造体メンバだけを初期化できる．
static struct file_operations tactsw_fops = {
  .read = tactsw_read,
  .ioctl = tactsw_ioctl,
  .open = tactsw_open,
  .release = tactsw_release,
};

static int __init tactsw_setup(int major)
{
  int i, error, gpio, irq;
  tactsw_info.major = major;
  tactsw_info.nbuttons = sizeof(tactsw_buttons)/sizeof(int);
  tactsw_info.buttons = tactsw_buttons;
  tactsw_info.used = 0;
  tactsw_info.mlen = 0;
  init_waitqueue_head(&(tactsw_info.wq));
  spin_lock_init(&(tactsw_info.slock));
  for (i = 0; i < tactsw_info.nbuttons; i++) {
    gpio = tactsw_info.buttons[i];
    error = gpio_request(gpio, "tactsw");
    // 2nd arg (label) is used for debug message and sysfs.
    if (error < 0) {
      printk("tactsw: gpio_request error %d (GPIO=%d)\n", error, gpio);
      goto fail;
    }

    error = gpio_direction_input(gpio);
    if (error < 0) {
      printk("tactsw: gpio_direction_input error %d (GPIO=%d)\n", error, gpio);
      goto free_fail;
    }

    irq = gpio_to_irq(gpio);
    if (irq < 0) {
      error = irq;
      printk("tactsw: gpio_to_irq error %d (GPIO=%d)\n", error, gpio);
      goto free_fail;
    }

    error = request_irq(irq, tactsw_intr,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"tactsw",	// used for debug message
			tactsw_intr);	// passed to isr's 2nd arg
    if (error) {
      printk("tactsw: request_irq error %d (IRQ=%d)\n", error, irq);
      goto free_fail;
    }
  }	// end of for
  return 0;
  
 free_fail:
  gpio_free(gpio);

 fail:
  while (--i >= 0) {
    gpio = tactsw_info.buttons[i];
    free_irq(gpio_to_irq(gpio), tactsw_intr);
    gpio_free(gpio);
  }
  return error;
}

// ・staticと付けるとこのファイル内でのみ見えることの指示．
//   付けないとOS全体から見えるようになる．シンボルテーブルが取られる．
//   OSがリブートしない限りシンボルテーブル上で邪魔になる．
// ・init __init とすると .text.initセクションに入る．
//   不要になった時 (=init終了後) に削除してくれる．
static int __init tactsw_init(void)
{
  int ret, major;
  dev_t dev = MKDEV(0, 0);	// dev_tは単なるint
  ret = alloc_chrdev_region(&dev, 0, N_TACTSW, "tactsw");
  if (ret < 0) {
  printk("tactsw_init_-1\n");
	return -1;
  }
  major = MAJOR(dev);
  printk("tactsw: Major number = %d.\n", major);
  cdev_init(&tactsw_dev, &tactsw_fops);
  tactsw_dev.owner = THIS_MODULE;
  ret = cdev_add(&tactsw_dev, MKDEV(major, 0), N_TACTSW);
  if (ret < 0) {
	printk("tactsw: cdev_add error\n");
	unregister_chrdev_region(dev, N_TACTSW);
	printk("tactsw_init_-1\n");
	return -1;
  }

  ret = tactsw_setup(major);
  if (ret < 0) {
    printk("tactsw: setup error\n");
    cdev_del(&tactsw_dev);
    unregister_chrdev_region(dev, N_TACTSW);
  }
  return ret;
}

// init __exit は上と同様に .text.exit セクションに入る．
static void __exit tactsw_exit(void)
{
  dev_t dev=MKDEV(tactsw_info.major, 0);
  int i;
  // disable interrupts

  for (i = 0; i < tactsw_info.nbuttons; i++) {
    int gpio = tactsw_info.buttons[i];
    int irq = gpio_to_irq(gpio);
    free_irq(irq, tactsw_intr);
    gpio_free(gpio);
  }

  // delete devices
  cdev_del(&tactsw_dev);
  unregister_chrdev_region(dev, N_TACTSW);

  // wake up tasks
  // This case never occurs since OS rejects rmmod when the device is open.
  if (waitqueue_active(&(tactsw_info.wq))) {
    printk("tactsw: there remains waiting tasks.  waking up.\n");
    wake_up_all(&(tactsw_info.wq));
    // Strictly speaking, we have to wait all processes wake up.
  }
}

module_init(tactsw_init);
module_exit(tactsw_exit);

MODULE_AUTHOR("Project6");
MODULE_DESCRIPTION("tact switch driver for armadillo-440");
MODULE_LICENSE("GPL");
