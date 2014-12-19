/*
 * Copyright 2013-2014, MathWorks, Inc.
 *
 */


#include "mwadma.h"  /* IOCTL */
#include <linux/amba/xilinx_dma.h>

dev_t mwadma_dev_id = 0;


static unsigned int device_num = 0;
static unsigned int cur_minor = 0;
static size_t channel_offset;
static struct class *mwadma_class = NULL;

char * mwdma_buf;
dma_addr_t mwdma_addr;
/*
 * Forward declaration of functions
 */
/*************************************************************************/
static int mw_axidma_setupchannel(struct mwadma_dev *mwdev,
        struct mwadma_chan *mwchan,
        struct mw_axidma_params *usrbuf);

static int mw_axidma_alloc(struct mwadma_dev *mwdev, size_t bufferSize);

static int mwadma_mmap(struct file *fp, struct vm_area_struct *vma);

static void mwadma_free_channel(struct mwadma_dev *mwdev,
        struct mwadma_chan *mwchan);

static int mwadma_channel_probe(struct mwadma_dev *mwdev);

static void mwdma_test_loopback(struct mwadma_dev * mwdev,
        struct mw_axidma_params chn_prm);

int mwadma_start(struct mwadma_dev *mwdev,struct mwadma_chan *mwchan);

/*************************************************************************/
/*
 * @brief mwadma_reg_write
 */
void mwadma_reg_write(struct mwadma_dev *mwdev, unsigned reg, unsigned val)
{
    iowrite32(val, mwdev->regs + reg);
}

/*
 * @brief mwadma_reg_read
 */
unsigned int mwadma_reg_read(struct mwadma_dev *mwdev, unsigned reg)
{
    return (ioread32(mwdev->regs + reg));
}

/*
 * @brief mwadma_fasync_impl
 */
static int mwadma_fasync_impl(int fd, struct file* fp, int mode)
{
    struct mwadma_dev *mwdev = fp->private_data;
    return fasync_helper(fd, fp, mode, &mwdev->asyncq);
}

/*
 * @brief mwadma_open
 */
static int mwadma_open(struct inode *inode, struct file *fp)
{
    struct mwadma_dev *mwdev;
    if (inode == NULL)
    {
        MW_DBG_text("INODE is NULL\n");
    }
    mwdev = container_of(inode->i_cdev, struct mwadma_dev, cdev);
    fp->private_data = mwdev;

    return 0;
}

/*
 * @brief mwadma_filterfn
 */
static bool mwadma_filterfn(struct dma_chan *chan, void *param)
{
   MW_DBG_printf(":chan->private=0x%p, chan_match=%d <--> param=0x%p, param_match=%d\n", chan->private,(int)chan->private, param, *(int*)param);
   if ((int)chan->private == *(int*)param) {
       return true;
   }
   return false;
}


/*
 * @brief mwadma_allocate_desc
 */
static int mwadma_allocate_desc(struct mwadma_slist **new, struct mwadma_chan *mwchan, unsigned int idx)
{
    struct scatterlist *this_sg;
    struct mwadma_slist * tmp;
    void *sg_buff;
    int ret, i = 0;
    size_t ring_bytes;
 
    ring_bytes = mwchan->length/mwchan->ring_total;
    tmp = (struct mwadma_slist *)kmalloc(sizeof(struct mwadma_slist),GFP_KERNEL);
    tmp->status = BD_UNALLOC;
    tmp->sg_t = (struct sg_table *)kmalloc(sizeof(struct sg_table),GFP_KERNEL);
    if (tmp->sg_t == NULL) {
        printk(KERN_ERR DRIVER_NAME ": Error in sgtable KMALLOC\n");
        return -ENOMEM;
    }
    ret = sg_alloc_table(tmp->sg_t, mwchan->sg_entries, GFP_ATOMIC);
    if (ret) {
        printk(KERN_ERR DRIVER_NAME ": Error in sg_alloc_table\n");
        sg_free_table(tmp->sg_t);
        return -ENOMEM;
    }
    if (mwchan->buf == NULL) {
        tmp->buf = (char*)__get_free_pages(GFP_KERNEL|__GFP_ZERO, get_order(ring_bytes));
        printk(KERN_ERR DRIVER_NAME ":Channel buffer was null. This should never happen.");
    }
    else {
        /* set buffer at offset from larger buffer */
        tmp->buf = &mwchan->buf[idx * ring_bytes];
        // printk(KERN_INFO DRIVER_NAME ":tmp->buf:0x%p\n",tmp->buf);
        tmp->buffer_index = idx;
    }
    for_each_sg(tmp->sg_t->sgl, this_sg, mwchan->sg_entries, i)
    {
        sg_buff = &(tmp->buf[(mwchan->bd_bytes)*i]);
        sg_set_buf(this_sg,sg_buff,mwchan->bd_bytes);
        // printk(KERN_INFO DRIVER_NAME ":sg-buff:0x%p\n",sg_buff);
    }
    tmp->status = BD_ALLOC;
    *new = tmp;
    return 0;
}

/*
 * @brief mwadma_unmap_desc
 */
static void mwadma_unmap_desc(struct mwadma_dev *mwdev, struct mwadma_chan *mwchan, struct mwadma_slist *input_slist)
{
    dma_unmap_sg(&IP2DEV(mwdev), input_slist->sg_t->sgl, mwchan->sg_entries, mwchan->direction);
    input_slist->status = BD_UNALLOC;
}

/*
 * @brief mwadma_map_desc
 */
static int mwadma_map_desc(struct mwadma_dev *mwdev, struct mwadma_chan *mwchan, struct mwadma_slist *ipsl)
{
    int retVal;
    retVal = dma_map_sg(&IP2DEV(mwdev), ipsl->sg_t->sgl, mwchan->sg_entries, mwchan->direction);
    if (retVal == 0)  {
        dev_err(&IP2DEV(mwdev),"no buffers available\n");
        ipsl->status = BD_UNALLOC;
        return -ENOMEM;
    }
    ipsl->desc = dmaengine_prep_slave_sg(mwchan->chan, ipsl->sg_t->sgl, mwchan->sg_entries, mwchan->direction, mwchan->flags);
    if (ipsl->desc == NULL) {
        mwadma_unmap_desc(mwdev, mwchan, ipsl);
        dev_err(&IP2DEV(mwdev), "Failed to prep slave\n");
        ipsl->status = BD_UNALLOC;
        retVal = -ENOMEM;
        return -ENOMEM;
    }
    ipsl->status = BD_MAPPED;
    return 0;
}

/*
 * @brief mwadma_prep_desc
 */
static int mwadma_prep_desc(struct mwadma_dev *mwdev, struct mwadma_chan * mwchan)
{
    unsigned int i = 0;
    int ret;
    struct mwadma_slist *new;

    /* First BD RING */
    ret = mwadma_allocate_desc(&(mwchan->scatter), mwchan, 0);
    if (ret < 0){
        dev_err(&IP2DEV(mwdev), "Failed in mwadma_allocate_desc");
        return -ENOMEM;
    }
    mwadma_map_desc(mwdev, mwchan, mwchan->scatter);
    /* New List of BD RING */
    INIT_LIST_HEAD(&(mwchan->scatter->list));
    for(i = 1; i < mwchan->ring_total; i++) /* POOL_SIZE - 1 */
    {
        ret = mwadma_allocate_desc(&(new), mwchan, i);
        if ((ret < 0) || (new == NULL)) {
            dev_err(&IP2DEV(mwdev), "Failed in mwadma_allocate_desc");
            return -ENOMEM;
        }
        list_add_tail(&(new->list),&(mwchan->scatter->list));
        mwadma_map_desc(mwdev, mwchan, new);
    }
    mwchan->curr = list_entry(mwchan->scatter->list.prev, struct mwadma_slist, list); /*Before first index (last index)*/
    mwchan->prev = list_entry(mwchan->curr->list.prev, struct mwadma_slist, list);
    return 0;
}

void mwadma_tx_cb_single_signal(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->tx;
    struct device *dev = &IP2DEV(mwdev);
    static int ct = 1;
    
    mwchan->transfer_queued--;
    /* Signal userspace */
    if (likely(mwdev->asyncq))
    {
        /* kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT); */
    }
    mwchan->transfer_count++;
    mwchan->status = ready;
    dev_dbg(dev, "Notify from %s : count:%d\n",__func__,ct++);
    sysfs_notify(&dev->kobj, NULL, "dma_ch2");
}

void mwadma_tx_cb_continuous_signal_dataflow(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->tx;

    mwchan->transfer_queued--;
    MW_DBG_printf( ": Queue fill level = %ld\n",mwchan->transfer_queued);

    if(mwchan->transfer_queued > 0)
    {
        mwadma_start(mwdev,mwchan);

        if(mwchan->transfer_queued > TX_WATERMARK_QFULL) /* High watermark */
        {
            mwchan->error = TX_ERROR_QFULL;
            kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);
        }
        else if(mwchan->transfer_queued > TX_WATERMARK_QPRIME) /* Normal */
        {
            mwchan->error = TX_ERROR_QPRIME;
            /*kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);*/
        }
        else if(mwchan->transfer_queued >= TX_WATERMARK_QLOW) /* Low */
        {
            mwchan->error = TX_ERROR_QLOW;
            kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);
        }

    }
    else /* Underflow */
    {
        mwchan->error = TX_ERROR_QUNDERFLOW;
        kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);
        mwchan->status = waiting;
        MW_DBG_text( ": Underflow condition\n");
    }
    mwchan->transfer_count++;
}


void mwadma_tx_cb_continuous_signal(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->tx;
    mwchan->transfer_queued--;
    mwadma_start(mwdev,mwchan);
    if (likely(mwdev->asyncq))
    {
        kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);
    }
    mwchan->transfer_count++;
}


void mwadma_rx_cb_single_signal(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->rx;
    struct device *dev = &IP2DEV(mwdev);
    static int ct = 1;
    
    mwchan->completed = mwchan->prev;    
    /* Signal userspace */    
    if (likely(mwdev->asyncq))
    {
        /* kill_fasync(&mwdev->asyncq, SIGIO, POLL_IN); */
        if(mwchan->next_index == mwchan->prev->buffer_index)
        {
            mwchan->error = ERR_RING_OVERFLOW;
            dev_err(&IP2DEV(mwdev), "Overflow condition occurred:%s at %d\n", __func__, __LINE__);
        }
    }
    else
    {
        mwchan->next_index = mwchan->prev->buffer_index;
    }
    mwchan->transfer_count++;
    mwchan->status = ready;
#ifdef DEBUG_IN_RATE
    mwchan->stop = ktime_get(); 
#endif
    dev_dbg(dev, "Notify from %s : count:%d\n",__func__,ct++);
    sysfs_notify(&dev->kobj, NULL, "dma_ch1");
}

void mwadma_rx_cb_burst(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->rx;
    mwchan->completed = mwchan->prev;
    mwchan->transfer_queued--;
    if (mwchan->transfer_queued)
    {
        mwadma_start(mwdev,mwchan);
    }
    else
    {
        if (likely(mwdev->asyncq))
        {
            kill_fasync(&mwdev->asyncq, SIGIO, POLL_IN);
        }
        else
        {
            mwchan->next_index = mwchan->prev->buffer_index;
        }
    }
    /*MW_DBG_printf( "Completed buffer index = %d\n",mwchan->completed->buffer_index);*/
    mwchan->transfer_count++;
#ifdef DEBUG_IN_RATE
    mwchan->stop = ktime_get(); 
#endif
}


void mwadma_rx_cb_continuous_signal(struct mwadma_dev *mwdev)
{
    struct mwadma_chan *mwchan = mwdev->rx;
    /*mwchan->completed = list_entry(mwchan->prev->list.prev,struct mwadma_slist,list);*/
    
    // complete(&mwchan->dma_complete);
    mwchan->completed = mwchan->prev;
    mwchan->transfer_count++;
    mwadma_start(mwdev,mwchan);
    /* Signal userspace */
    if (likely(mwdev->asyncq))
    {
        kill_fasync(&mwdev->asyncq, SIGIO, POLL_IN);
    }

    if(mwchan->transfer_count == 1) /* First transfer */
    {
        mwchan->next_index = mwchan->completed->buffer_index;
    }
    else if(mwchan->next_index == mwchan->completed->buffer_index)
    {
        mwchan->error = ERR_RING_OVERFLOW;
        dev_err(&IP2DEV(mwdev), "Overflow condition:%s at %d\n", __func__, __LINE__);
    }
#ifdef DEBUG_IN_RATE
    mwchan->stop = ktime_get(); 
#endif
}

/*
 * @brief mwadma_start
 */
int mwadma_start(struct mwadma_dev *mwdev,struct mwadma_chan *mwchan)
{
    int ret = 0;
    struct mwadma_slist *new;
    if(mwdev == NULL) {
        return -ENODEV;
    }
    /* Get next ring for transfer from the pool */
    new = list_entry(mwchan->curr->list.next,struct mwadma_slist,list);
    /* Fresh buffer or has been used previously */
    if((new->status == BD_MAPPED) || (new->status == BD_PROCESS))
    {
        /*dev_dbg(&IP2DEV(mwdev),"mwchan:0x%p, mwchan->chan:0x%p, DMA_CHAN:%s\n", \
                mwchan, mwchan->chan, dma_chan_name(mwchan->chan));        
         */
        new->desc->callback = mwchan->callback;
        new->desc->callback_param = mwdev;
        new->cookie = dmaengine_submit(new->desc);
        if (dma_submit_error(new->cookie)) {
			dev_err(&IP2DEV(mwdev), "Failure to submit cookie\n");
            ret = -EINVAL;
			return ret;
		}
        mwchan->curr->desc = dmaengine_prep_slave_sg(mwchan->chan, mwchan->curr->sg_t->sgl, mwchan->sg_entries, mwchan->direction, mwchan->flags);
        if (mwchan->curr->desc == NULL)
        {
            mwadma_unmap_desc(mwdev, mwchan, mwchan->curr);
            dev_err(&IP2DEV(mwdev), "Failed to prep slave\n");
            ret = -ENOMEM;
            return ret;
        }
        mwchan->prev = mwchan->curr;
        mwchan->curr = new;
        mwchan->prev->status = BD_PROCESS;
        return 0;
    }
    return ret;
}

/*
 * @brief mwadma_stop
 */
static int mwadma_stop(struct mwadma_dev *mwdev, struct mwadma_chan *mwchan)
{
    struct xilinx_dma_config config;
    
    config.coalesc = 0;
    config.delay = 0;
    dmaengine_device_control(mwchan->chan, DMA_TERMINATE_ALL, (unsigned long)&config);
    // xilinx_dma_reset(mwchan->chan); /* make DMA engine aware that last transfer is interrupted */
    dev_dbg(&IP2DEV(mwdev),"DMA STOP\nIterations = %lu\n",mwchan->transfer_count);
#ifdef DEBUG_IN_RATE
    dev_dbg(&IP2DEV(mwdev),"DMA transfers time = %lld ns\n", (long long)ktime_to_ns(ktime_sub(mwchan->stop,mwchan->start)));
#endif
    return 0;
}

/*
 * @brief mwadma_rx_ctl
 */
static long mwadma_rx_ctl(struct mwadma_dev *mwdev, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    unsigned long userval;
    struct mw_axidma_params usrbuf;
    switch(cmd)
    {
        case MWADMA_SETUP_RX_CHANNEL:
            if(copy_from_user(&usrbuf, (struct mw_axidma_params *)arg, sizeof(struct mw_axidma_params)))
            {
                return -EACCES;
            }
            if (mwdev->rx == NULL)
            {
                return -ENOMEM;
            }
            ret = mw_axidma_setupchannel(mwdev, mwdev->rx, &usrbuf);
            break;
        case MWADMA_RX_SINGLE:
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            
            switch(userval)
            {
                case SIGNAL_TRANSFER_COMPLETE: 
                    mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_single_signal;
                    break;
                default:
                    mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_continuous_signal;
            }
            
            spin_lock(&mwdev->rx->slock);
            mwdev->rx->error = 0;
            mwdev->rx->transfer_count = 0;
            mwadma_start(mwdev, mwdev->rx);
            dma_async_issue_pending(mwdev->rx->chan);
            spin_unlock(&mwdev->rx->slock);
            // check_completion(mwdev,mwdev->rx);
            break;
        case MWADMA_RX_BURST:
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_burst;
            /* Start from the first */
            if(userval > mwdev->rx->ring_total)
            {
                return -EINVAL;
            }
            mwdev->rx->transfer_queued = userval;
            mwdev->rx->transfer_count = 0;
            mwdev->rx->error = 0;
            mwadma_start(mwdev,mwdev->rx);
            dma_async_issue_pending(mwdev->rx->chan);
            break;
        case MWADMA_RX_CONTINUOUS:
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            switch(userval)
            {
                case SIGNAL_TRANSFER_COMPLETE: 
                    mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_continuous_signal;
                    break;
                default:
                    mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_continuous_signal;
                    //dev_dbg(&IP2DEV(mwdev), "No mode specified for Rx continuous\n");
            }
            /* Start from the first */
            mwdev->rx->transfer_count = 0;
            mwdev->rx->error = 0;
            mwadma_start(mwdev,mwdev->rx);
            dma_async_issue_pending(mwdev->rx->chan);
            mwadma_start(mwdev,mwdev->rx);
            mwdev->rx->status = running;
            break;
        case MWADMA_RX_STOP:
            spin_lock(&mwdev->rx->slock);
            if(mwdev->rx->status == running)
            {
                ret = mwadma_stop(mwdev,mwdev->rx);
                if (ret) {
                    printk(KERN_ERR "Error while stopping DMA\n");
                    return ret;
                }
                kill_fasync(&mwdev->asyncq, SIGIO, POLL_IN);
                dev_err(&IP2DEV(mwdev),"Partial transfer\n");
            }
            mwdev->rx->status = ready;
            spin_unlock(&mwdev->rx->slock);
            break;
        case MWADMA_RX_GET_NEXT_INDEX:
            if(copy_to_user((unsigned long *) arg, &mwdev->rx->next_index, sizeof(unsigned long)))
            {
                return -EACCES;
            }
            mwdev->rx->next_index = (mwdev->rx->next_index + 1) % mwdev->rx->buffer_interrupts;
            break;
        case MWADMA_RX_GET_ERROR:
            if(copy_to_user((unsigned long *) arg, &mwdev->rx->error, sizeof(unsigned long)))
            {
                return -EACCES;
            }
            mwdev->rx->error = 0;
            break;
        case MWADMA_FREE_RX_CHANNEL:
            //            mwadma_free_channel(mwdev, mwdev->rx);
            break;
        default:
            return 1;
    }
    return 0;
}


/*
 * @brief mwadma_tx_ctl
 */
static long mwadma_tx_ctl(struct mwadma_dev *mwdev, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    unsigned long userval;
    struct mw_axidma_params usrbuf;
    struct mwadma_slist *new;
    switch(cmd)
    {
        case MWADMA_SETUP_TX_CHANNEL:
            if(copy_from_user(&usrbuf, (struct mw_axidma_params *)arg, sizeof(struct mw_axidma_params)))
            {
                return -EACCES;
            }
            // mwdev->tx = (struct mwadma_chan*)devm_kzalloc(&IP2DEV(mwdev),sizeof(mwdev->tx),GFP_KERNEL);
            if (mwdev->tx  == NULL)
            {
                return -ENOMEM;
            }
            ret = mw_axidma_setupchannel(mwdev, mwdev->tx, &usrbuf);
            break;
            
        case MWADMA_TX_ENQUEUE:
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            spin_lock(&mwdev->tx->slock);
            if((mwdev->tx->transfer_queued + userval) >= mwdev->tx->ring_total)
            {
                dev_err(&IP2DEV(mwdev), \
                        ":queue:%lu, user-queue:%lu, ring:%u\n", \
                        mwdev->tx->transfer_queued, \
                        userval, \
                        mwdev->tx->ring_total);
                mwdev->tx->error = TX_ERROR_QFULL;
                // kill_fasync(&mwdev->asyncq, SIGIO, POLL_OUT);
                spin_unlock(&mwdev->tx->slock);
                return 0;
            }
            mwdev->tx->transfer_queued += userval;
            if(unlikely((mwdev->tx->status == waiting) && (mwdev->tx->transfer_queued > TX_WATERMARK_QPRIME))) /* restart if required */
            {
                mwadma_start(mwdev,mwdev->tx);
                dma_async_issue_pending(mwdev->tx->chan);
                mwadma_start(mwdev,mwdev->tx);
                mwdev->tx->status = running; /*Data ready */
                dev_dbg(&IP2DEV(mwdev),"Fill level reached\n");
            }
            spin_unlock(&mwdev->tx->slock);
            break;
        case MWADMA_TX_SINGLE:
            
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            switch(userval)
            {
                case SIGNAL_TRANSFER_COMPLETE: 
                    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_single_signal;
                    break;
                default:
                    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_single_signal;
            }
            if (!mwdev->tx->transfer_queued)
            {
                dev_err(&IP2DEV(mwdev),"Queue is empty\n");
                spin_unlock(&mwdev->tx->slock);
                return -EINVAL;
            }
            spin_lock(&mwdev->tx->slock);
            mwdev->tx->next_index = (mwdev->tx->next_index + 1) % mwdev->tx->ring_total;            
            mwadma_start(mwdev,mwdev->tx);
            dma_async_issue_pending(mwdev->tx->chan);
            spin_unlock(&mwdev->tx->slock);
            // check_completion(mwdev, mwdev->tx);
            break;
        case MWADMA_TX_CONTINUOUS:
            if(copy_from_user(&userval, (unsigned long *)arg, sizeof(userval)))
            {
                return -EACCES;
            }
            spin_lock(&mwdev->tx->slock);
            switch(userval)
            {
                case SIGNAL_TRANSFER_COMPLETE: 
                    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_continuous_signal;
                    break;
                case SIGNAL_DATAFLOW: 
                    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_continuous_signal_dataflow;
                    break;
                default:
                    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_continuous_signal;
            }
            mwdev->tx->status = waiting; /* Wait on queued data */
            mwdev->tx->next_index = (mwdev->tx->next_index + 1) % mwdev->tx->ring_total;
            spin_unlock(&mwdev->tx->slock);
            break;
        case MWADMA_TX_STOP:
            spin_lock(&mwdev->tx->slock);
            if(mwdev->tx->status == running)
            {

                ret = mwadma_stop(mwdev,mwdev->tx);
                if (ret)
                {
                    dev_err(&IP2DEV(mwdev),"Error while stopping DMA\n");
                    spin_unlock(&mwdev->tx->slock);
                    return ret;
                }
                mwdev->tx->status = ready;
            }
            mwdev->tx->transfer_queued = 0; /* Reset pending transfers */
            spin_unlock(&mwdev->tx->slock);
            break;
        case MWADMA_TX_GET_ERROR:
            /* MW_DBG_printf( "Requested Tx error status = %d\n",mwdev->tx->error);
             */
            if(copy_to_user((unsigned long *) arg, &mwdev->tx->error, sizeof(unsigned long)))
            {
                return -EACCES;
            }
            mwdev->tx->error = 0;
            break;
        case MWADMA_TX_GET_NEXT_INDEX:
            dev_dbg(&IP2DEV(mwdev), "Requested Tx error status = %d\n",mwdev->tx->error);
            spin_lock(&mwdev->tx->slock);
            new = list_entry(mwdev->tx->curr->list.next,struct mwadma_slist,list);
            userval = (unsigned long)new->buffer_index;
            spin_unlock(&mwdev->tx->slock);
            if(copy_to_user((unsigned long *) arg, &userval, sizeof(unsigned long)))
            {
                return -EACCES;
            }
            break;            
        case MWADMA_FREE_TX_CHANNEL:
            // mwadma_free_channel(mwdev, mwdev->tx);
            break;
        default:
            return 1;
    }
    return 0;
}

/*
 * @brief mwadma_generic_ctl
 */
static long mwadma_generic_ctl(struct mwadma_dev *mwdev, unsigned int cmd, unsigned long arg)
{
    struct mw_axidma_params usrbuf;
    switch(cmd)
    {
        case MWADMA_GET_PROPERTIES:
            usrbuf.size = mwdev->size;
            usrbuf.phys = (dma_addr_t)mwdev->phys;
            if(copy_to_user((struct mw_axidma_params *)arg, &usrbuf, sizeof(struct mw_axidma_params))) {
                return -EACCES;
            }
            break;
        case MWADMA_TEST_LOOPBACK:
            if(copy_from_user(&usrbuf, (struct mw_axidma_params *)arg, sizeof(struct mw_axidma_params)))
            {
                return -EACCES;
            }
            mwdma_test_loopback(mwdev, usrbuf);
            break;
        default:
            return 1;
    }
    return 0;
}

/*
 * @brief mwadma_ioctl
 */
static long mwadma_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
    int ret_rx = 0;
    int ret_tx = 0;
    int ret_generic = 0;
    struct mwadma_dev *mwdev = fp->private_data;

    if (NULL == mwdev)
    {
        return -ENODEV;
    }

    ret_rx = mwadma_rx_ctl(mwdev,cmd,arg);
    ret_tx = mwadma_tx_ctl(mwdev,cmd,arg);
    ret_generic = mwadma_generic_ctl(mwdev,cmd,arg);
    /* Errors */
    if(ret_rx < 0)
    {
        return ret_rx;
    }
    if(ret_tx < 0) 
    {
        return ret_tx;
    }
    if(ret_generic < 0)
    {
        return ret_generic;
    }

    /* No valid case found */
    if(3 == (ret_rx + ret_tx + ret_generic))
    {
        dev_dbg(&IP2DEV(mwdev), "Invalid ioctl: command: %u\n", cmd);
        return -EINVAL;
    }
    return 0;
}


/*
 * @brief mwadma_close
 */
static int mwadma_close(struct inode *inode, struct file *fp)
{
    struct mwadma_dev *mwdev = fp->private_data;
    int ret = 0;

    /* MW_DBG_printf( ":Closing the file-descriptor for %s\n", DRIVER_NAME); */

    if (NULL == mwdev)
    {
        return -ENODEV;
    }
    mwadma_fasync_impl(-1, fp, 0);
    return ret;
}

/*
 * @brief mwadma_mmap_dma_open
 */
static void mwadma_mmap_dma_open(struct vm_area_struct *vma)
{
    struct mwadma_dev * mwdev = vma->vm_private_data;
	dev_info(&IP2DEV(mwdev), "DMA VMA open, virt %lx, phys %lx \n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
    
}

/*
 * @brief mwadma_free_channel
 */
static void mwadma_free_channel(struct mwadma_dev *mwdev, struct mwadma_chan *mwchan)
{
    struct mwadma_slist *slist, *_slist;
    struct xilinx_dma_config config;

    unsigned long flags;
    spin_lock_irqsave(&mwchan->slock, flags);
    list_for_each_entry_safe(slist, _slist, &mwchan->scatter->list, list) {
        mwadma_unmap_desc(mwdev, mwchan, slist);
        sg_free_table(slist->sg_t);
        kfree(slist->sg_t);
        list_del(&slist->list);
        kfree(&slist->list);
    }
    dmaengine_device_control(mwchan->chan, DMA_TERMINATE_ALL, (unsigned long)&config);
    // xilinx_dma_reset(mwchan->chan);
    dma_release_channel(mwchan->chan);
    spin_unlock_irqrestore(&mwchan->slock, flags);
    dev_dbg(&IP2DEV(mwdev), "MWADMA Free channel done.");
}

/*
 * @brief mwadma_mmap_dma_close
 */
static void mwadma_mmap_dma_close(struct vm_area_struct *vma)
{
    struct mwadma_dev * mwdev = vma->vm_private_data;
	dev_info(&IP2DEV(mwdev), "DMA VMA close.\n");
	/* Free the memory DMA */
    if (mwdev->size) {
        dev_info(&IP2DEV(mwdev), "free dma memory.\n");
        dma_free_coherent(&IP2DEV(mwdev), mwdev->size, mwdev->virt, mwdev->phys);
        mwdev->size = 0;
        channel_offset = 0;
        mwdev->virt = NULL;
        mwdev->phys = 0;
    }
}

/*
 * @brief mwadma_mmap_open
 */
static void mwadma_mmap_open(struct vm_area_struct *vma)
{
    struct mwadma_dev * mwdev = vma->vm_private_data;
	dev_info(&IP2DEV(mwdev), "Simple VMA open, virt %lx, phys %lx \n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

/*
 * @brief mwadma_mmap_close
 */
static void mwadma_mmap_close(struct vm_area_struct *vma)
{
    struct mwadma_dev * mwdev = vma->vm_private_data;
	dev_info(&IP2DEV(mwdev ), "Simple VMA close.\n");
}

/*
 * @brief mwadma_mmap_fault
 */
static int mwadma_mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    struct mwadma_dev * mwdev = vma->vm_private_data;
    struct page *thisPage;
    unsigned long offset;
    offset = (vmf->pgoff - vma->vm_pgoff) << PAGE_SHIFT;
    thisPage = virt_to_page(mwdev->mem->start + offset);
    get_page(thisPage);
    vmf->page = thisPage;
    return 0;
}

struct vm_operations_struct  mwadma_mmap_ops = {
    .open           = mwadma_mmap_open,
    .close          = mwadma_mmap_close,
    .fault          = mwadma_mmap_fault,
}; 

struct vm_operations_struct mwadma_mmap_dma_ops = {
    .open           = mwadma_mmap_dma_open,
    .close          = mwadma_mmap_dma_close,
};


struct file_operations mwadma_cdev_fops = {
    .owner = THIS_MODULE,
    .open = mwadma_open,
    .fasync = mwadma_fasync_impl,
    .release = mwadma_close,
    .mmap		    = mwadma_mmap,
    .unlocked_ioctl = mwadma_ioctl,
};

/*
 * @brief mwadma_mmap
 */
static int mwadma_mmap(struct file *fp, struct vm_area_struct *vma)
{
    struct mwadma_dev *mwdev = fp->private_data;
    size_t size = vma->vm_end - vma->vm_start;
    int status = 0;
    vma->vm_private_data = mwdev;
    dev_info(&IP2DEV(mwdev), "[MMAP] size:%X pgoff: %lx\n", size, vma->vm_pgoff);
 
    switch(vma->vm_pgoff) {
		case 0: 
			/* mmap the Memory Mapped I/O's base address */
                        vma->vm_flags |= VM_IO | VM_DONTDUMP;
			vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
			if (remap_pfn_range(vma, vma->vm_start,
					mwdev->mem->start >> PAGE_SHIFT,
					size,
					vma->vm_page_prot))
			{
				return -EAGAIN;
			}
			vma->vm_ops = &mwadma_mmap_ops;
			break;
		default:
                      /* mmap the DMA region */
                      status = mw_axidma_alloc(mwdev, size);
                     if ((status) && (status != -EEXIST))  {
                         return -ENOMEM;
                     }
                    dev_dbg(&IP2DEV(mwdev), "dma setup_cdev successful\n");

                    status = 0;
			if (mwdev->virt == NULL){
                            return -EINVAL;
                        }
                    vma->vm_pgoff = 0;
                    status = dma_mmap_coherent(&IP2DEV(mwdev), vma, mwdev->virt,
                        mwdev->phys, mwdev->size);
                    if (status) {
                        dev_dbg(&IP2DEV(mwdev),"Remapping memory failed, error: %d\n", status);
                        return status;
                    }
                    vma->vm_ops = &mwadma_mmap_dma_ops;
                    dev_dbg(&IP2DEV(mwdev),"%s: mapped dma addr 0x%08lx at 0x%08lx, size %d\n",
                          __func__, (unsigned long)mwdev->phys, vma->vm_start,
                          mwdev->size);
                     break;
    }
	return status;
}


/*
 * @brief mw_axidma_alloc
 */
static int mw_axidma_alloc(struct mwadma_dev *mwdev, size_t bufferSize)
{
    if (mwdev == NULL)
    {
        return -ENOMEM;
    }
    if (mwdev->virt != NULL) 
    {
		dev_err(&IP2DEV(mwdev), "DMA memory already allocated\n");		
		return -EEXIST;
	}
    mwdev->virt = dma_alloc_coherent(&IP2DEV(mwdev), bufferSize, \
            &mwdev->phys, \
            GFP_KERNEL);
    if (mwdev->virt == NULL)
    {
        dev_err(&IP2DEV(mwdev), "Failed to allocate continguous memory\nUsing multiple buffers\n");
    }
    
    else {
        dev_info(&IP2DEV(mwdev), "Address of buffer = 0x%p, Length = %u Bytes\n",\
                (void *)virt_to_phys(mwdev->virt),bufferSize);
        mwdev->size = bufferSize;
    }
    return 0;
}

/*
 * @brief mw_axidma_setupchannel
 */
static int mw_axidma_setupchannel(struct mwadma_dev *mwdev, 
        struct mwadma_chan *mwchan,
        struct mw_axidma_params *usrbuf)
{
    int status = 0;
    static int idx = 0;
    struct xilinx_dma_config    config;
    if ( (mwdev == NULL) || (mwchan == NULL) ) {
        return -EINVAL;
    }
    mwchan->flags               = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    mwchan->ring_total          = usrbuf->total_rings;
    mwchan->length              = usrbuf->bytes_per_ring * usrbuf->total_rings;
    
    mwchan->bd_bytes            = usrbuf->desc_length;
    mwchan->sg_entries          = usrbuf->bytes_per_ring/usrbuf->desc_length;
    mwchan->buffer_interrupts   = mwchan->ring_total;
    
    /* Write to the IPCore_PacketSize_AXI4_Stream_Master 0x8 to specify the length*/
    /*reset pcore*/
    mwadma_reg_write(mwdev, 0x0, 0x1);
    /*reset pcore*/
    mwadma_reg_write(mwdev, 0x8, usrbuf->counter);
    mwchan->buf                 = &(mwdev->virt[channel_offset]);
    if (mwchan->buf == NULL) {
        dev_err(&IP2DEV(mwdev), "Buffer is NULL. Failed to allocate memory\n");
        return -ENOMEM;
	}
    mwchan->offset              =  channel_offset;
    channel_offset              =  channel_offset + mwchan->length;
    /*
     * Set channel-index : used to notify appropriate DMA_CHX SYFS node
     */
    mwchan->chan_id             =  idx;
    idx++;
    dev_dbg(&IP2DEV(mwdev), "### Printing Channel info...\n");
    dev_dbg(&IP2DEV(mwdev), "Virtual Address        :0x%p\n", mwchan->buf);
    dev_dbg(&IP2DEV(mwdev), "Channel Length/Size    :%lu\n", mwchan->length);
    dev_dbg(&IP2DEV(mwdev), "Channel direction      :%d\n", mwchan->direction);
    dev_dbg(&IP2DEV(mwdev), "Total number of rings  :%d\n", mwchan->ring_total);
    dev_dbg(&IP2DEV(mwdev), "Buffer Descriptor size :%d\n", mwchan->bd_bytes);
    dev_dbg(&IP2DEV(mwdev), "Channel SG Entries     :%d\n", mwchan->sg_entries);
    dev_dbg(&IP2DEV(mwdev), "Buffer Interrupts      :%d\n", mwchan->buffer_interrupts);  
    /* Get channel for DMA */
    mutex_init(&mwchan->lock);
    config.coalesc = 0;
    config.delay = 0;
    dmaengine_device_control(mwchan->chan, DMA_SLAVE_CONFIG, (unsigned long)&config);
    dev_dbg(&IP2DEV(mwdev),"Name:%s, mwchan:0x%p, mwchan->chan:0x%p\n", 
            dma_chan_name(mwchan->chan), mwchan, mwchan->chan);
    
    if (mwchan->ring_total >= 2) {
        status = mwadma_prep_desc(mwdev, mwchan);
    } else
    {
        /*
         * Instantiate scatter structure, it contains cookie, desc, callback
         */
        mwchan->scatter = devm_kzalloc(&IP2DEV(mwdev), 
                sizeof(struct mwadma_slist), GFP_KERNEL);
    }
    init_completion(&mwchan->dma_complete);
    spin_lock_init(&mwchan->slock);
    return status;
}

/*
 * @brief mwadma_setup_cdev
 */
static int mwadma_setup_cdev(struct mwadma_dev *mwdev, dev_t *dev_id)
{
    int status = 0;
    struct device * thisDevice;
    cdev_init(&mwdev->cdev, &mwadma_cdev_fops);
    mwdev->cdev.owner = THIS_MODULE;
    mwdev->cdev.ops = &mwadma_cdev_fops;
    *dev_id = MKDEV(MAJOR(mwadma_dev_id), cur_minor++);
    status = cdev_add(&mwdev->cdev, *dev_id, 1);

    if (status < 0) {
        return status;
    }
    thisDevice = device_create(mwadma_class, NULL, *dev_id, NULL, "%s", DRIVER_NAME);
    if(IS_ERR(thisDevice))
    {
        status = PTR_ERR(thisDevice);
        dev_err(&IP2DEV(mwdev), "Error: failed to create device node %s, err %d\n", mwdev->name, status);
        cdev_del(&mwdev->cdev);
    }
    return status;
}

static void mwdma_test_loopback(struct mwadma_dev * mwdev, 
        struct mw_axidma_params chan_prm)
{
    int i = 0;
    size_t len;
    char *dma_addr = mwdev->virt;  
    unsigned int *tmp;
    /* rx = &dma_addr[0];
     * tx = &dma_addr[chan_prm.size];
     */
    dev_dbg(&IP2DEV(mwdev),"### test loopback\n");

    len = chan_prm.size;
    /* prime the rx & tx buffers */
    tmp = (unsigned int *) dma_addr;
    for (i=0;i<(len/sizeof(unsigned int));i++)
    {
        tmp[i] = 0xDEADC0DE;
    }
    tmp = (unsigned int *) (dma_addr + len);
    for (i=0;i<(len/sizeof(unsigned int));i++)
    {
        tmp[i] = (i+1) % (chan_prm.bytes_per_ring/sizeof(unsigned int));
    }
    /* Receive single ring */
    mwdev->rx->callback = (dma_async_tx_callback)mwadma_rx_cb_single_signal;
    mwdev->rx->error = 0;
    mwdev->rx->transfer_count = 0;
    mwadma_start(mwdev, mwdev->rx);
    dma_async_issue_pending(mwdev->rx->chan);
    /* Transmit single ring */
    mwdev->tx->transfer_queued += 1;
    mwdev->tx->callback = (dma_async_tx_callback)mwadma_tx_cb_single_signal;
    mwdev->tx->next_index = (mwdev->tx->next_index + 1) % mwdev->tx->ring_total;
    mwadma_start(mwdev,mwdev->tx);
    dma_async_issue_pending(mwdev->tx->chan);
}


static ssize_t mwdma_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    dev_dbg(dev,"sysfs_notify :%s\n", attr->attr.name);
    // sysfs_notify(&dev->kobj, NULL, attr->attr.name);
    return (sizeof(int));
}

static ssize_t mwdma_show(struct device *dev, struct device_attribute *attr,
        char *buf)
{
    dev_dbg(dev, "Showing channel %s\n",attr->attr.name);
    return sprintf(buf, "%s\n", attr->attr.name);
}

static DEVICE_ATTR(dma_ch1, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch2, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch3, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch4, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch5, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch6, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch7, S_IRUGO, mwdma_show, mwdma_store);
static DEVICE_ATTR(dma_ch8, S_IRUGO, mwdma_show, mwdma_store);

static struct attribute *mwdma_attributes[] = {
    &dev_attr_dma_ch1.attr,
    &dev_attr_dma_ch2.attr,
    &dev_attr_dma_ch3.attr,
    &dev_attr_dma_ch4.attr,
    &dev_attr_dma_ch5.attr,
    &dev_attr_dma_ch6.attr,
    &dev_attr_dma_ch7.attr,
    &dev_attr_dma_ch8.attr,
    NULL,
};

static const struct attribute_group mwdma_attr_group = {
    .attrs = mwdma_attributes,
};

static int mwadma_channel_probe(struct mwadma_dev *mwdev)
{
    struct dma_chan *rxchan,*txchan;
    static char id = 0;
    dma_cap_mask_t mask;

    u32 match_rx, match_tx;
    
    match_tx  = (DMA_MEM_TO_DEV & 0xFF) | XILINX_DMA_IP_DMA;
    match_rx  = (DMA_DEV_TO_MEM & 0xFF) | XILINX_DMA_IP_DMA;
    
    dma_cap_zero(mask);
    dma_cap_set(DMA_SLAVE , mask);
    dma_cap_set(DMA_PRIVATE, mask);
    dma_cap_set(DMA_CYCLIC, mask);
    
    if (id > 1) {
        dev_dbg(&IP2DEV(mwdev), "Exceeded maximum allowable RX-TX DMA channel pairs\n");
        return -ENOMEM;
    }
    
    while (1) {
       dev_dbg(&IP2DEV(mwdev), "requesting tx-channel\n");
       txchan = dma_request_channel(mask, mwadma_filterfn,
                (void *)&match_tx);
       dev_dbg(&IP2DEV(mwdev), "requesting rx-channel\n");
       rxchan = dma_request_channel(mask, mwadma_filterfn,
                (void *)&match_rx);
          if (!txchan && !rxchan) {
            dev_dbg(&IP2DEV(mwdev), "no more channels found!\n");
            break;
        } else {
            dev_dbg(&IP2DEV(mwdev), "some channels found!\n");
            if (txchan) {
                mwdev->tx = (struct mwadma_chan*)devm_kzalloc(&IP2DEV(mwdev), 
                        sizeof(struct mwadma_chan),GFP_KERNEL);
                if (mwdev->tx == NULL) {
                    dev_err(&IP2DEV(mwdev), "failed to allocate tx channel\n");
                    return -ENODEV;
                }
                mwdev->tx->chan = txchan;
                mwdev->tx->direction = DMA_MEM_TO_DEV; 
            }
            if (rxchan) {
                mwdev->rx = (struct mwadma_chan*)devm_kzalloc(&IP2DEV(mwdev), 
                        sizeof(struct mwadma_chan),GFP_KERNEL);
                if (mwdev->rx == NULL) {
                    dev_err(&IP2DEV(mwdev), "failed to allocate rx channel\n");
                    return -ENODEV;
                }
                mwdev->rx->chan = rxchan;
                mwdev->rx->direction = DMA_DEV_TO_MEM;
            }
            id++;
        }
    }   
    return 0;
}
/*
 * @brief mwadma_of_probe
 */
static int mwadma_of_probe(struct platform_device *op)
{
    int                         status = 0;
    struct mwadma_dev           *mwdev;
    struct device_node          *np = op->dev.of_node;
    struct of_phandle_args      dma_spec;
    struct device *dev  = &op->dev;

    mwdev = (struct mwadma_dev*)devm_kzalloc(&op->dev, sizeof(struct mwadma_dev),GFP_KERNEL);
    if (!mwdev) {
        status = -ENOMEM;
        goto allocation_error;
    }
    mwdev->mem = platform_get_resource(op, IORESOURCE_MEM,0);
    if(!mwdev->mem) {
        status = -ENOENT;
        dev_err(&op->dev, "Failed to get resource for platform device\n");
        goto invalid_platform_res;
    }
    dev_dbg(&op->dev, "Dev memory resource found at 0x%08X 0x%08X.\n", mwdev->mem->start, resource_size(mwdev->mem));
    mwdev->mem = request_mem_region(mwdev->mem->start, resource_size(mwdev->mem), op->name);
    if(!mwdev->mem) {
        status = -EBUSY;
        dev_err(&op->dev, "Failed to get mem region for our device\n");
        goto invalid_platform_res;
    }
    mwdev->regs = ioremap(mwdev->mem->start, resource_size(mwdev->mem));
    if(!mwdev->regs) {
        status = -ENODEV;
        dev_err(&op->dev, "Failed to do ioremap\n");
        goto ioremap_failed;
    }
    mwdev->pdev = op;
    mwdev->name = np->name;
    if(np->data == NULL) {
        np->data = mwdev;
    }
    if(mwadma_dev_id == 0) {
        status = alloc_chrdev_region(&mwadma_dev_id, 0, 16, DRIVER_NAME);
        if (status)
        {
            dev_err(&op->dev, "Character dev. region not allocated: %d\n", status);
            goto chrdev_alloc_err;
        }
        dev_dbg(&op->dev, "Char dev region registered: major num:%d\n", MAJOR(mwadma_dev_id));
    }
    if(mwadma_class == NULL) {
        mwadma_class = class_create(THIS_MODULE, DRIVER_NAME);
        if(IS_ERR(mwadma_class))
        {
            status = PTR_ERR(mwadma_class);
            goto class_create_err;
        }
        dev_dbg(&op->dev, "mwadma class registration success\n");
    }
    status = mwadma_setup_cdev(mwdev, &(mwdev->dev_id));
    if(status)
    {
        dev_err(&op->dev, "mwadma device addition failed: %d\n", status);
        goto dev_add_err;
    }
    
    dev_info(&op->dev, "pcore phys_addr:0x%08llX mapped to 0x%p\n", (unsigned long long)mwdev->mem->start, mwdev->regs);
    device_num++;
   
    /*
     * ####################################################################
     * Following section does probing & init of DMA channels
     * ####################################################################
     */
    
    /*  find associated AXI DMA  */
    status = of_parse_phandle_with_args(np, "dma-request", "#dma-cells",0, &dma_spec);
    if(status) {
        dev_info(&op->dev, "Device tree binding did not have dma-request. Continue as a MMIO device mapping\n");
	return 0;
    }
    dev_dbg(&IP2DEV(mwdev), "of_parse_handle_with_args successful\n");
    status = mwadma_channel_probe(mwdev);
    if (status){
        dev_err(&IP2DEV(mwdev),"Channel probe failed. Verify device tree and FPGA IP core addresses.\n");
        goto dev_add_err;        
    }
   
    /*
     * ####################################################################
     * Following section creates the sysfs entires for DMA
     * ####################################################################
     */
    status = sysfs_create_group(&dev->kobj, &mwdma_attr_group);
    if (status) {
        dev_err(&op->dev, "Error creating the sysfs devices\n");
        goto dev_add_err;
    }
    return status;
    
dev_add_err:
    if(mwadma_class){
        class_destroy(mwadma_class);
    }
class_create_err:
    unregister_chrdev_region(mwadma_dev_id, 16);
    mwadma_dev_id = 0;
chrdev_alloc_err:
    iounmap(mwdev->regs);
ioremap_failed:
    release_mem_region(mwdev->mem->start, resource_size(mwdev->mem));
invalid_platform_res:

allocation_error:
    return status;
}

/*
 * @brief mwadma_of_remove
 */
static int mwadma_of_remove(struct platform_device *pdev)
{
    struct mwadma_dev *mwdev;
    struct device *dev = &pdev->dev;
    struct device_node *np =  pdev->dev.of_node;
    if(np->data == NULL)
    {
        dev_err(&pdev->dev, "MWADMA device not found.\n");
        return -ENOSYS;
    }
    mwdev = (struct mwadma_dev *) (np->data);
    /* If user did not free up the channel and DMA memory, do it while we
     * unload the driver
     */
    dev_dbg(&IP2DEV(mwdev), "Freeing coherent dma memory\n");
    if(mwdev->virt != NULL)  {
        dma_free_coherent(&IP2DEV(mwdev),mwdev->size, mwdev->virt ,mwdev->phys);
    }
    dev_dbg(&IP2DEV(mwdev), "Freeing rx-scatter channel dma memory\n");
    if ((mwdev->rx !=NULL) && (mwdev->rx->scatter !=NULL) && (&mwdev->rx->scatter->list != NULL)) 
    {
        mwadma_free_channel(mwdev, mwdev->rx);
    }
    dev_dbg(&IP2DEV(mwdev), "Freeing tx-scatter channel dma memory\n");
    if ((mwdev->tx !=NULL) && (mwdev->tx->scatter !=NULL) && (&mwdev->tx->scatter->list != NULL)) 
    {
        mwadma_free_channel(mwdev, mwdev->tx);
    }
    MW_DBG_printf(KERN_INFO DRIVER_NAME "%s : free and release memory\n", np->name);    
    dev_info(&IP2DEV(mwdev),"Removing sysfs entries");
    sysfs_remove_group(&dev->kobj, &mwdma_attr_group);

    if(mwdev->regs) {
        iounmap(mwdev->regs);
    }
    if(mwdev->mem->start) {
        release_mem_region(mwdev->mem->start, resource_size(mwdev->mem));
    } else {
        dev_err(&pdev->dev, "Invalid address\n");
    }
    np->data = NULL;
    device_num--;
    if(&mwdev->cdev) {
        dev_dbg(&IP2DEV(mwdev),  "Destroy character dev\n");
        device_destroy(mwadma_class, mwdev->dev_id);
        cdev_del(&mwdev->cdev);
    }
    cur_minor--;
    if(device_num == 0)  {
        dev_dbg(&IP2DEV(mwdev),  "destroy mwadma class\n");
        if (mwadma_class) {
            class_destroy(mwadma_class);
        }
        dev_dbg(&IP2DEV(mwdev),  "release device region\n");
        unregister_chrdev_region(mwadma_dev_id, 16);
        mwadma_dev_id = 0;
    }
    return 0;
}

/*
 * @brief mwadma_of_match
 */
static const struct of_device_id mwadma_of_match[]  = {
    { .compatible = "mathworks,mwipcore-v2.00",},
    { .compatible = "mathworks,mwipcore-axi4lite-v1.00",},
    {},
};

static struct platform_driver mwadma_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = mwadma_of_match,
    },
    .probe = mwadma_of_probe,
    .remove = mwadma_of_remove,
};

module_platform_driver(mwadma_driver);

MODULE_DEVICE_TABLE(of, mwadma_of_match);
MODULE_AUTHOR("MathWorks, Inc");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME ": MathWorks AXI4-Lite/AXI4-Stream DMA driver");

/*DMA PARAMS */
