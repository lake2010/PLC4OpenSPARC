/* sunhv.c: Serial driver for SUN4V hypervisor console.
 *
 * Copyright (C) 2006, 2007 David S. Miller (davem@davemloft.net)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/circ_buf.h>
#include <linux/serial.h>
#include <linux/sysrq.h>
#include <linux/console.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/hypervisor.h>
#include <asm/spitfire.h>
#include <asm/prom.h>
#include <asm/of_device.h>
#include <asm/irq.h>

#if defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/serial_core.h>

#include "suncore.h"

#define CON_BREAK	((long)-1)
#define CON_HUP		((long)-2)

#define IGNORE_BREAK	0x1
#define IGNORE_ALL	0x2

static char *con_write_page;
static char *con_read_page;



#define OPENSPARC_T1_FPGA   1


#ifdef OPENSPARC_T1_FPGA

static int sunhv_console_polling_period = 5;

static int __init setup_sunhv_console_polling_period(char *str)
{
        get_option(&str, &sunhv_console_polling_period);
        return 1;
}
__setup("sunhv_console_polling_period=", setup_sunhv_console_polling_period);

static struct timer_list  sunhv_timer;

#endif /* #ifdef OPENSPARC_T1_FPGA */


static int hung_up = 0;

static void transmit_chars_putchar(struct uart_port *port, struct circ_buf *xmit)
{
	while (!uart_circ_empty(xmit)) {
		long status = sun4v_con_putchar(xmit->buf[xmit->tail]);

		if (status != HV_EOK)
			break;

		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
	}
}

static void transmit_chars_write(struct uart_port *port, struct circ_buf *xmit)
{
	while (!uart_circ_empty(xmit)) {
		unsigned long ra = __pa(xmit->buf + xmit->tail);
		unsigned long len, status, sent;

		len = CIRC_CNT_TO_END(xmit->head, xmit->tail,
				      UART_XMIT_SIZE);
		status = sun4v_con_write(ra, len, &sent);
		if (status != HV_EOK)
			break;
		xmit->tail = (xmit->tail + sent) & (UART_XMIT_SIZE - 1);
		port->icount.tx += sent;
	}
}

static int receive_chars_getchar(struct uart_port *port, struct tty_struct *tty)
{
	int saw_console_brk = 0;
	int limit = 10000;

	while (limit-- > 0) {
		long status;
		long c = sun4v_con_getchar(&status);

		if (status == HV_EWOULDBLOCK)
			break;

		if (c == CON_BREAK) {
			if (uart_handle_break(port))
				continue;
			saw_console_brk = 1;
			c = 0;
		}

		if (c == CON_HUP) {
			hung_up = 1;
			uart_handle_dcd_change(port, 0);
		} else if (hung_up) {
			hung_up = 0;
			uart_handle_dcd_change(port, 1);
		}

		if (tty == NULL) {
			uart_handle_sysrq_char(port, c);
			continue;
		}

		port->icount.rx++;

		if (uart_handle_sysrq_char(port, c))
			continue;

		tty_insert_flip_char(tty, c, TTY_NORMAL);
	}

	return saw_console_brk;
}

static int receive_chars_read(struct uart_port *port, struct tty_struct *tty)
{
	int saw_console_brk = 0;
	int limit = 10000;

	while (limit-- > 0) {
		unsigned long ra = __pa(con_read_page);
		unsigned long bytes_read, i;
		long stat = sun4v_con_read(ra, PAGE_SIZE, &bytes_read);

		if (stat != HV_EOK) {
			bytes_read = 0;

			if (stat == CON_BREAK) {
				if (uart_handle_break(port))
					continue;
				saw_console_brk = 1;
				*con_read_page = 0;
				bytes_read = 1;
			} else if (stat == CON_HUP) {
				hung_up = 1;
				uart_handle_dcd_change(port, 0);
				continue;
			} else {
				/* HV_EWOULDBLOCK, etc.  */
				break;
			}
		}

		if (hung_up) {
			hung_up = 0;
			uart_handle_dcd_change(port, 1);
		}

		for (i = 0; i < bytes_read; i++)
			uart_handle_sysrq_char(port, con_read_page[i]);

		if (tty == NULL)
			continue;

		port->icount.rx += bytes_read;

		tty_insert_flip_string(tty, con_read_page, bytes_read);
	}

	return saw_console_brk;
}

struct sunhv_ops {
	void (*transmit_chars)(struct uart_port *port, struct circ_buf *xmit);
	int (*receive_chars)(struct uart_port *port, struct tty_struct *tty);
};

static struct sunhv_ops bychar_ops = {
	.transmit_chars = transmit_chars_putchar,
	.receive_chars = receive_chars_getchar,
};

static struct sunhv_ops bywrite_ops = {
	.transmit_chars = transmit_chars_write,
	.receive_chars = receive_chars_read,
};

static struct sunhv_ops *sunhv_ops = &bychar_ops;

static struct tty_struct *receive_chars(struct uart_port *port)
{
	struct tty_struct *tty = NULL;

	if (port->info != NULL)		/* Unopened serial console */
		tty = port->info->tty;

	if (sunhv_ops->receive_chars(port, tty))
		sun_do_break();

	return tty;
}

static void transmit_chars(struct uart_port *port)
{
	struct circ_buf *xmit;

	if (!port->info)
		return;

	xmit = &port->info->xmit;
	if (uart_circ_empty(xmit) || uart_tx_stopped(port))
		return;

	sunhv_ops->transmit_chars(port, xmit);

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static irqreturn_t sunhv_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	struct tty_struct *tty;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	tty = receive_chars(port);
	transmit_chars(port);
	spin_unlock_irqrestore(&port->lock, flags);

	if (tty)
		tty_flip_buffer_push(tty);
 
	return IRQ_HANDLED;
}

/* port->lock is not held.  */
static unsigned int sunhv_tx_empty(struct uart_port *port)
{
	/* Transmitter is always empty for us.  If the circ buffer
	 * is non-empty or there is an x_char pending, our caller
	 * will do the right thing and ignore what we return here.
	 */
	return TIOCSER_TEMT;
}

/* port->lock held by caller.  */
static void sunhv_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	return;
}

/* port->lock is held by caller and interrupts are disabled.  */
static unsigned int sunhv_get_mctrl(struct uart_port *port)
{
	return TIOCM_DSR | TIOCM_CAR | TIOCM_CTS;
}

/* port->lock held by caller.  */
static void sunhv_stop_tx(struct uart_port *port)
{
	return;
}

/* port->lock held by caller.  */
static void sunhv_start_tx(struct uart_port *port)
{
	transmit_chars(port);
}

/* port->lock is not held.  */
static void sunhv_send_xchar(struct uart_port *port, char ch)
{
	unsigned long flags;
	int limit = 10000;

	spin_lock_irqsave(&port->lock, flags);

	while (limit-- > 0) {
		long status = sun4v_con_putchar(ch);
		if (status == HV_EOK)
			break;
		udelay(1);
	}

	spin_unlock_irqrestore(&port->lock, flags);
}

/* port->lock held by caller.  */
static void sunhv_stop_rx(struct uart_port *port)
{
}

/* port->lock held by caller.  */
static void sunhv_enable_ms(struct uart_port *port)
{
}

/* port->lock is not held.  */
static void sunhv_break_ctl(struct uart_port *port, int break_state)
{
	if (break_state) {
		unsigned long flags;
		int limit = 10000;

		spin_lock_irqsave(&port->lock, flags);

		while (limit-- > 0) {
			long status = sun4v_con_putchar(CON_BREAK);
			if (status == HV_EOK)
				break;
			udelay(1);
		}

		spin_unlock_irqrestore(&port->lock, flags);
	}
}

/* port->lock is not held.  */
static int sunhv_startup(struct uart_port *port)
{
	return 0;
}

/* port->lock is not held.  */
static void sunhv_shutdown(struct uart_port *port)
{
}

/* port->lock is not held.  */
static void sunhv_set_termios(struct uart_port *port, struct ktermios *termios,
			      struct ktermios *old)
{
	unsigned int baud = uart_get_baud_rate(port, termios, old, 0, 4000000);
	unsigned int quot = uart_get_divisor(port, baud);
	unsigned int iflag, cflag;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);

	iflag = termios->c_iflag;
	cflag = termios->c_cflag;

	port->ignore_status_mask = 0;
	if (iflag & IGNBRK)
		port->ignore_status_mask |= IGNORE_BREAK;
	if ((cflag & CREAD) == 0)
		port->ignore_status_mask |= IGNORE_ALL;

	/* XXX */
	uart_update_timeout(port, cflag,
			    (port->uartclk / (16 * quot)));

	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *sunhv_type(struct uart_port *port)
{
	return "SUN4V HCONS";
}

static void sunhv_release_port(struct uart_port *port)
{
}

static int sunhv_request_port(struct uart_port *port)
{
	return 0;
}

static void sunhv_config_port(struct uart_port *port, int flags)
{
}

static int sunhv_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	return -EINVAL;
}

static struct uart_ops sunhv_pops = {
	.tx_empty	= sunhv_tx_empty,
	.set_mctrl	= sunhv_set_mctrl,
	.get_mctrl	= sunhv_get_mctrl,
	.stop_tx	= sunhv_stop_tx,
	.start_tx	= sunhv_start_tx,
	.send_xchar	= sunhv_send_xchar,
	.stop_rx	= sunhv_stop_rx,
	.enable_ms	= sunhv_enable_ms,
	.break_ctl	= sunhv_break_ctl,
	.startup	= sunhv_startup,
	.shutdown	= sunhv_shutdown,
	.set_termios	= sunhv_set_termios,
	.type		= sunhv_type,
	.release_port	= sunhv_release_port,
	.request_port	= sunhv_request_port,
	.config_port	= sunhv_config_port,
	.verify_port	= sunhv_verify_port,
};

static struct uart_driver sunhv_reg = {
	.owner			= THIS_MODULE,
	.driver_name		= "serial",
	.dev_name		= "ttyS",
	.major			= TTY_MAJOR,
};

static struct uart_port *sunhv_port;

/* Copy 's' into the con_write_page, decoding "\n" into
 * "\r\n" along the way.  We have to return two lengths
 * because the caller needs to know how much to advance
 * 's' and also how many bytes to output via con_write_page.
 */
static int fill_con_write_page(const char *s, unsigned int n,
			       unsigned long *page_bytes)
{
	const char *orig_s = s;
	char *p = con_write_page;
	int left = PAGE_SIZE;

	while (n--) {
		if (*s == '\n') {
			if (left < 2)
				break;
			*p++ = '\r';
			left--;
		} else if (left < 1)
			break;
		*p++ = *s++;
		left--;
	}
	*page_bytes = p - con_write_page;
	return s - orig_s;
}

static void sunhv_console_write_paged(struct console *con, const char *s, unsigned n)
{
	struct uart_port *port = sunhv_port;
	unsigned long flags;
	int locked = 1;

	local_irq_save(flags);
	if (port->sysrq) {
		locked = 0;
	} else if (oops_in_progress) {
		locked = spin_trylock(&port->lock);
	} else
		spin_lock(&port->lock);

	while (n > 0) {
		unsigned long ra = __pa(con_write_page);
		unsigned long page_bytes;
		unsigned int cpy = fill_con_write_page(s, n,
						       &page_bytes);

		n -= cpy;
		s += cpy;
		while (page_bytes > 0) {
			unsigned long written;
			int limit = 1000000;

			while (limit--) {
				unsigned long stat;

				stat = sun4v_con_write(ra, page_bytes,
						       &written);
				if (stat == HV_EOK)
					break;
				udelay(1);
			}
			if (limit <= 0)
				break;
			page_bytes -= written;
			ra += written;
		}
	}

	if (locked)
		spin_unlock(&port->lock);
	local_irq_restore(flags);
}

static inline void sunhv_console_putchar(struct uart_port *port, char c)
{
	int limit = 1000000;

	while (limit-- > 0) {
		long status = sun4v_con_putchar(c);
		if (status == HV_EOK)
			break;
		udelay(1);
	}
}

static void sunhv_console_write_bychar(struct console *con, const char *s, unsigned n)
{
	struct uart_port *port = sunhv_port;
	unsigned long flags;
	int i, locked = 1;

	local_irq_save(flags);
	if (port->sysrq) {
		locked = 0;
	} else if (oops_in_progress) {
		locked = spin_trylock(&port->lock);
	} else
		spin_lock(&port->lock);

	spin_lock_irqsave(&port->lock, flags);
	for (i = 0; i < n; i++) {
		if (*s == '\n')
			sunhv_console_putchar(port, '\r');
		sunhv_console_putchar(port, *s++);
	}

	if (locked)
		spin_unlock(&port->lock);
	local_irq_restore(flags);
}

static struct console sunhv_console = {
	.name	=	"ttyHV",
	.write	=	sunhv_console_write_bychar,
	.device	=	uart_console_device,
	.flags	=	CON_PRINTBUFFER,
	.index	=	-1,
	.data	=	&sunhv_reg,
};


#ifdef OPENSPARC_T1_FPGA
static void sunhv_console_poll(unsigned long data)
{
	struct uart_port *port = sunhv_port;
	struct tty_struct *tty;
	unsigned long flags;

	tty = receive_chars(port);
	transmit_chars(port);

	if (tty)
		tty_flip_buffer_push(tty);

	mod_timer(&sunhv_timer, jiffies + sunhv_console_polling_period);
}
#endif /* ifdef OPENSPARC_T1_FPGA */


static int __devinit hv_probe(struct of_device *op, const struct of_device_id *match)
{
	struct uart_port *port;
	unsigned long minor;
	int err;

	if (op->irqs[0] == 0xffffffff)
		return -ENODEV;

	port = kzalloc(sizeof(struct uart_port), GFP_KERNEL);
	if (unlikely(!port))
		return -ENOMEM;

	minor = 1;
	if (sun4v_hvapi_register(HV_GRP_CORE, 1, &minor) == 0 &&
	    minor >= 1) {
		err = -ENOMEM;
		con_write_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!con_write_page)
			goto out_free_port;

		con_read_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!con_read_page)
			goto out_free_con_write_page;

		sunhv_console.write = sunhv_console_write_paged;
		sunhv_ops = &bywrite_ops;
	}

	sunhv_port = port;

	port->line = 0;
	port->ops = &sunhv_pops;
	port->type = PORT_SUNHV;
	port->uartclk = ( 29491200 / 16 ); /* arbitrary */

	port->membase = (unsigned char __iomem *) __pa(port);

	port->irq = op->irqs[0];

	port->dev = &op->dev;

	sunhv_reg.minor = sunserial_current_minor;
	sunhv_reg.nr = 1;

	err = uart_register_driver(&sunhv_reg);
	if (err)
		goto out_free_con_read_page;

	sunhv_reg.tty_driver->name_base = sunhv_reg.minor - 64;
	sunserial_current_minor += 1;

	sunserial_console_match(&sunhv_console, op->node,
				&sunhv_reg, port->line);

	err = uart_add_one_port(&sunhv_reg, port);
	if (err)
		goto out_unregister_driver;

#ifndef OPENSPARC_T1_FPGA

	err = request_irq(port->irq, sunhv_interrupt, 0, "hvcons", port);
	if (err)
		goto out_remove_port;

#else /* ifndef OPENSPARC_T1_FPGA */

	/* Interrupts are not generated by the console device. Therefore polling is used. */

        init_timer(&sunhv_timer);
	sunhv_timer.function = sunhv_console_poll;
	mod_timer(&sunhv_timer, jiffies + sunhv_console_polling_period);

#endif /* ifndef OPENSPARC_T1_FPGA */


	dev_set_drvdata(&op->dev, port);

	return 0;

out_remove_port:
	uart_remove_one_port(&sunhv_reg, port);

out_unregister_driver:
	sunserial_current_minor -= 1;
	uart_unregister_driver(&sunhv_reg);

out_free_con_read_page:
	kfree(con_read_page);

out_free_con_write_page:
	kfree(con_write_page);

out_free_port:
	kfree(port);
	sunhv_port = NULL;
	return err;
}

static int __devexit hv_remove(struct of_device *dev)
{
	struct uart_port *port = dev_get_drvdata(&dev->dev);

	free_irq(port->irq, port);

	uart_remove_one_port(&sunhv_reg, port);

	sunserial_current_minor -= 1;
	uart_unregister_driver(&sunhv_reg);

	kfree(port);
	sunhv_port = NULL;

	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

static struct of_device_id hv_match[] = {
	{
		.name = "console",
		.compatible = "qcn",
	},
	{
		.name = "console",
		.compatible = "SUNW,sun4v-console",
	},
	{},
};
MODULE_DEVICE_TABLE(of, hv_match);

static struct of_platform_driver hv_driver = {
	.name		= "hv",
	.match_table	= hv_match,
	.probe		= hv_probe,
	.remove		= __devexit_p(hv_remove),
};

static int __init sunhv_init(void)
{
	if (tlb_type != hypervisor)
		return -ENODEV;

	return of_register_driver(&hv_driver, &of_bus_type);
}

static void __exit sunhv_exit(void)
{
	of_unregister_driver(&hv_driver);
}

module_init(sunhv_init);
module_exit(sunhv_exit);

MODULE_AUTHOR("David S. Miller");
MODULE_DESCRIPTION("SUN4V Hypervisor console driver");
MODULE_VERSION("2.0");
MODULE_LICENSE("GPL");