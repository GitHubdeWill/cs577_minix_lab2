/* The kernel call implemented in this file:
 *   m_type:	SYS_VDEVIO
 *
 * The parameters for this kernel call are:
 *    m2_i3:	DIO_REQUEST	(request input or output)	
 *    m2_i1:	DIO_TYPE	(flag indicating byte, word, or long)
 *    m2_p1:	DIO_VEC_ADDR	(pointer to port/ value pairs)	
 *    m2_i2:	DIO_VEC_SIZE	(number of ports to read or write) 
 */

#include "../system.h"
#include <minix/devio.h>
#include <minix/endpoint.h>
#include <minix/portio.h>

#if USE_VDEVIO

/* Buffer for SYS_VDEVIO to copy (port,value)-pairs from/ to user. */
PRIVATE char vdevio_buf[VDEVIO_BUF_SIZE];      
PRIVATE pvb_pair_t *pvb = (pvb_pair_t *) vdevio_buf;           
PRIVATE pvw_pair_t *pvw = (pvw_pair_t *) vdevio_buf;      
PRIVATE pvl_pair_t *pvl = (pvl_pair_t *) vdevio_buf;     

/*===========================================================================*
 *			        do_vdevio                                    *
 *===========================================================================*/
PUBLIC int do_vdevio(m_ptr)
register message *m_ptr;	/* pointer to request message */
{
/* Perform a series of device I/O on behalf of a non-kernel process. The 
 * I/O addresses and I/O values are fetched from and returned to some buffer
 * in user space. The actual I/O is wrapped by lock() and unlock() to prevent
 * that I/O batch from being interrrupted.
 * This is the counterpart of do_devio, which performs a single device I/O. 
 */ 
  int vec_size;               /* size of vector */
  int io_in;                  /* true if input */
  size_t bytes;               /* # bytes to be copied */
  vir_bytes caller_vir;       /* virtual address at caller */
  phys_bytes caller_phys;     /* physical address at caller */
  port_t port;
  int i, j, io_size, nr_io_range;
  int io_dir, io_type;
  struct proc *rp;
  struct priv *privp;
  struct io_range *iorp;
    
  /* Get the request, size of the request vector, and check the values. */
  io_dir = m_ptr->DIO_REQUEST & _DIO_DIRMASK;
  io_type = m_ptr->DIO_REQUEST & _DIO_TYPEMASK;
  if (io_dir == _DIO_INPUT) io_in = TRUE;
  else if (io_dir == _DIO_OUTPUT) io_in = FALSE;
  else return(EINVAL);
  if ((vec_size = m_ptr->DIO_VEC_SIZE) <= 0) return(EINVAL);
  switch (io_type) {
      case _DIO_BYTE:
	bytes = vec_size * sizeof(pvb_pair_t);
	io_size= sizeof(u8_t);
	break;
      case _DIO_WORD:
	bytes = vec_size * sizeof(pvw_pair_t);
	io_size= sizeof(u16_t);
	break;
      case _DIO_LONG:
	bytes = vec_size * sizeof(pvl_pair_t);
	io_size= sizeof(u32_t);
	break;
      default:  return(EINVAL);   /* check type once and for all */
  }
  if (bytes > sizeof(vdevio_buf))  return(E2BIG);

  /* Calculate physical addresses and copy (port,value)-pairs from user. */
  caller_vir = (vir_bytes) m_ptr->DIO_VEC_ADDR;
  caller_phys = umap_local(proc_addr(who_p), D, caller_vir, bytes);
  if (0 == caller_phys) return(EFAULT);
  phys_copy(caller_phys, vir2phys(vdevio_buf), (phys_bytes) bytes);

  rp= proc_addr(who_p);
  privp= priv(rp);
  if (privp && (privp->s_flags & CHECK_IO_PORT))
  {
	/* Check whether the I/O is allowed */
	nr_io_range= privp->s_nr_io_range;
	for (i=0; i<vec_size; i++)
	{
		switch (io_type) {
		case _DIO_BYTE: port= pvb[i].port; break;
		case _DIO_WORD: port= pvw[i].port; break;
		default:	port= pvl[i].port; break;
		}
		for (j= 0, iorp= privp->s_io_tab; j<nr_io_range; j++, iorp++)
		{
			if (port >= iorp->ior_base &&
				port+io_size-1 <= iorp->ior_limit)
			{
				break;
			}
		}
		if (j >= nr_io_range)
		{
			kprintf(
		"do_vdevio: I/O port check failed for proc %d, port 0x%x\n",
				m_ptr->m_source, port);
			return EPERM;
		}
	}
  }

  /* Perform actual device I/O for byte, word, and long values. Note that 
   * the entire switch is wrapped in lock() and unlock() to prevent the I/O
   * batch from being interrupted. 
   */  
  lock(13, "do_vdevio");
  switch (io_type) {
  case _DIO_BYTE: 					 /* byte values */
      if (io_in) for (i=0; i<vec_size; i++) 
		pvb[i].value = inb( pvb[i].port); 
      else      for (i=0; i<vec_size; i++)
		outb( pvb[i].port, pvb[i].value); 
      break; 
  case _DIO_WORD:					  /* word values */
      if (io_in)
      {
	for (i=0; i<vec_size; i++)  
	{
		port= pvw[i].port;
		if (port & 1) goto bad;
		pvw[i].value = inw( pvw[i].port);  
	}
      }
      else
      {
	for (i=0; i<vec_size; i++) 
	{
		port= pvw[i].port;
		if (port & 1) goto bad;
		outw( pvw[i].port, pvw[i].value); 
	}
      }
      break; 
  default:            					  /* long values */
      if (io_in)
      {
	for (i=0; i<vec_size; i++)
	{
		port= pvl[i].port;
		if (port & 3) goto bad;
		pvl[i].value = inl(pvl[i].port);  
	}
      }
      else
      {
	for (i=0; i<vec_size; i++)
	{
		port= pvl[i].port;
		if (port & 3) goto bad;
		outl( pvb[i].port, pvl[i].value); 
	}
      }
  }
  unlock(13);
    
  /* Almost done, copy back results for input requests. */
  if (io_in) phys_copy(vir2phys(vdevio_buf), caller_phys, (phys_bytes) bytes);
  return(OK);

bad:
	panic("do_vdevio: unaligned port\n", port);
	return EPERM;
}

#endif /* USE_VDEVIO */

