/************************************************************************************
 * arch/arm/src/tiva/tiva_i2c.c
 *
 *   Copyright (C) 2014 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Derives in spirit if nothing more from the NuttX STM32 I2C driver which has:
 *
 *   Copyright (C) 2011 Uros Platise. All rights reserved.
 *   Author: Uros Platise <uros.platise@isotel.eu>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/i2c.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>

#include <arch/board/board.h>

#include "up_arch.h"

#include "tiva_gpio.h"
#include "chip/tiva_pinmap.h"
#include "chip/tiva_syscontrol.h"
#include "tiva_i2c.h"

/* At least one I2C peripheral must be enabled */

#if defined(CONFIG_TIVA_I2C0) || defined(CONFIG_TIVA_I2C1) || \
    defined(CONFIG_TIVA_I2C2) || defined(CONFIG_TIVA_I2C3) || \
    defined(CONFIG_TIVA_I2C4) || defined(CONFIG_TIVA_I2C5)

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/
/* Configuration ********************************************************************/
/* CONFIG_I2C_POLLED may be set so that I2C interrupts will not be used.  Instead,
 * CPU-intensive polling will be used.
 */

/* Interrupt wait timeout in seconds and milliseconds */

#if !defined(CONFIG_TIVA_I2CTIMEOSEC) && !defined(CONFIG_TIVA_I2CTIMEOMS)
#  define CONFIG_TIVA_I2CTIMEOSEC 0
#  define CONFIG_TIVA_I2CTIMEOMS  500   /* Default is 500 milliseconds */
#elif !defined(CONFIG_TIVA_I2CTIMEOSEC)
#  define CONFIG_TIVA_I2CTIMEOSEC 0     /* User provided milliseconds */
#elif !defined(CONFIG_TIVA_I2CTIMEOMS)
#  define CONFIG_TIVA_I2CTIMEOMS  0     /* User provided seconds */
#endif

/* Interrupt wait time timeout in system timer ticks */

#ifndef CONFIG_TIVA_I2CTIMEOTICKS
#  define CONFIG_TIVA_I2CTIMEOTICKS \
    (SEC2TICK(CONFIG_TIVA_I2CTIMEOSEC) + MSEC2TICK(CONFIG_TIVA_I2CTIMEOMS))
#endif

#ifndef CONFIG_TIVA_I2C_DYNTIMEO_STARTSTOP
#  define CONFIG_TIVA_I2C_DYNTIMEO_STARTSTOP TICK2USEC(CONFIG_TIVA_I2CTIMEOTICKS)
#endif

/* Macros to convert a I2C pin to a GPIO output */

#define I2C_INPUT  (GPIO_FUNC_INPUT)
#define I2C_OUTPUT (GPIO_FUNC_ODOUTPUT | GPIO_PADTYPE_OD | GPIO_VALUE_ONE)

#define MKI2C_INPUT(p)  (((p) & (GPIO_PORT_MASK | GPIO_PIN_MASK)) | I2C_INPUT)
#define MKI2C_OUTPUT(p) (((p) & (GPIO_PORT_MASK | GPIO_PIN_MASK)) | I2C_OUTPUT)

/* Debug ****************************************************************************/
/* CONFIG_DEBUG_I2C + CONFIG_DEBUG enables general I2C debug output. */

#ifdef CONFIG_DEBUG_I2C
#  define i2cdbg dbg
#  define i2cvdbg vdbg
#else
#  define i2cdbg(x...)
#  define i2cvdbg(x...)
#endif

/* I2C event trace logic.  NOTE:  trace uses the internal, non-standard, low-level
 * debug interface syslog() but does not require that any other debug
 * is enabled.
 */

#ifndef CONFIG_I2C_TRACE
#  define tiva_i2c_tracereset(p)
#  define tiva_i2c_tracenew(p,s)
#  define tiva_i2c_traceevent(p,e,a)
#  define tiva_i2c_tracedump(p)
#endif

#ifndef CONFIG_I2C_NTRACE
#  define CONFIG_I2C_NTRACE 32
#endif

/************************************************************************************
 * Private Types
 ************************************************************************************/
/* Interrupt state */

enum tiva_intstate_e
{
  INTSTATE_IDLE = 0,      /* No I2C activity */
  INTSTATE_WAITING,       /* Waiting for completion of interrupt activity */
  INTSTATE_DONE,          /* Interrupt activity complete */
};

/* Trace events */

enum tiva_trace_e
{
  I2CEVENT_NONE = 0,      /* No events have occurred with this status */
  I2CEVENT_SENDADDR,      /* Start/Master bit set and address sent, param = msgc */
  I2CEVENT_SENDBYTE,      /* Send byte, param = dcnt */
  I2CEVENT_ITBUFEN,       /* Enable buffer interrupts, param = 0 */
  I2CEVENT_RCVBYTE,       /* Read more dta, param = dcnt */
  I2CEVENT_REITBUFEN,     /* Re-enable buffer interrupts, param = 0 */
  I2CEVENT_DISITBUFEN,    /* Disable buffer interrupts, param = 0 */
  I2CEVENT_BTFNOSTART,    /* BTF on last byte with no restart, param = msgc */
  I2CEVENT_BTFRESTART,    /* Last byte sent, re-starting, param = msgc */
  I2CEVENT_BTFSTOP,       /* Last byte sten, send stop, param = 0 */
  I2CEVENT_ERROR          /* Error occurred, param = 0 */
};

/* Trace data */

struct tiva_trace_s
{
  uint32_t status;             /* I2C 32-bit SR2|SR1 status */
  uint32_t count;              /* Interrupt count when status change */
  enum tiva_intstate_e event;  /* Last event that occurred with this status */
  uint32_t parm;               /* Parameter associated with the event */
  uint32_t time;               /* First of event or first status */
};

/* I2C Device hardware configuration */

struct tiva_i2c_config_s
{
  uintptr_t base;             /* I2C base address */
#ifndef TIVA_SYSCON_RCGCI2C
  uint32_t rcgbit;            /* Bits in RCG1 register to enable clocking */
#endif
  uint32_t scl_pin;           /* GPIO configuration for SCL as SCL */
  uint32_t sda_pin;           /* GPIO configuration for SDA as SDA */
#ifndef CONFIG_I2C_POLLED
  int (*isr)(int, void *);    /* Interrupt handler */
  uint8_t irq;                /* IRQ number */
#endif
  uint8_t devno;              /* I2Cn where n = devno */
};

/* I2C Device Private Data */

struct tiva_i2c_priv_s
{
  const struct tiva_i2c_config_s *config; /* Port configuration */
  int refs;                    /* Reference count */
  sem_t exclsem;              /* Mutual exclusion semaphore */
#ifndef CONFIG_I2C_POLLED
  sem_t waitsem;               /* Interrupt wait semaphore */
#endif
  volatile uint8_t intstate;   /* Interrupt handshake (see enum tiva_intstate_e) */

  uint8_t msgc;                /* Message count */
  struct i2c_msg_s *msgv;      /* Message list */
  uint8_t *ptr;                /* Current message buffer */
  int dcnt;                    /* Current message length */
  uint16_t flags;              /* Current message flags */

  /* I2C trace support */

#ifdef CONFIG_I2C_TRACE
  int tndx;                    /* Trace array index */
  uint32_t start_time;         /* Time when the trace was started */

  /* The actual trace data */

  struct tiva_trace_s trace[CONFIG_I2C_NTRACE];
#endif
};

/* I2C Device, Instance */

struct tiva_i2c_inst_s
{
  const struct i2c_ops_s *ops;  /* Standard I2C operations */
  struct tiva_i2c_priv_s *priv; /* Common driver private data structure */

  uint32_t    frequency;   /* Frequency used in this instantiation */
  int         address;     /* Address used in this instantiation */
  uint16_t    flags;       /* Flags used in this instantiation */
};

/************************************************************************************
 * Private Function Prototypes
 ************************************************************************************/

static inline uint32_t tiva_i2c_getreg(struct tiva_i2c_priv_s *priv,
                                       unsigned int offset);
static inline void tiva_i2c_putreg(struct tiva_i2c_priv_s *priv,
                                   unsigned int offset, uint32_t value);
static inline void tiva_i2c_modifyreg(struct tiva_i2c_priv_s *priv,
                                      uint8_t offset, uint16_t clearbits,
                                      uint16_t setbits);
static inline void tiva_i2c_sem_wait(struct i2c_dev_s *dev);

#ifdef CONFIG_TIVA_I2C_DYNTIMEO
static useconds_t tiva_i2c_tousecs(int msgc, struct i2c_msg_s *msgs);
#endif /* CONFIG_TIVA_I2C_DYNTIMEO */

static inline int  tiva_i2c_sem_waitdone(struct tiva_i2c_priv_s *priv);
static inline void tiva_i2c_sem_waitstop(struct tiva_i2c_priv_s *priv);
static inline void tiva_i2c_sem_post(struct i2c_dev_s *dev);
static inline void tiva_i2c_sem_init(struct i2c_dev_s *dev);
static inline void tiva_i2c_sem_destroy(struct i2c_dev_s *dev);

#ifdef CONFIG_I2C_TRACE
static void tiva_i2c_tracereset(struct tiva_i2c_priv_s *priv);
static void tiva_i2c_tracenew(struct tiva_i2c_priv_s *priv, uint32_t status);
static void tiva_i2c_traceevent(struct tiva_i2c_priv_s *priv,
                                enum tiva_trace_e event, uint32_t parm);
static void tiva_i2c_tracedump(struct tiva_i2c_priv_s *priv);
#endif /* CONFIG_I2C_TRACE */

static inline void tiva_i2c_sendstart(struct tiva_i2c_priv_s *priv);
static inline void tiva_i2c_clrstart(struct tiva_i2c_priv_s *priv);
static inline void tiva_i2c_sendstop(struct tiva_i2c_priv_s *priv);

static int tiva_i2c_interrupt(struct tiva_i2c_priv_s * priv, uint32_t status);

#ifndef CONFIG_I2C_POLLED
#ifdef CONFIG_TIVA_I2C0
static int tiva_i2c0_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_TIVA_I2C1
static int tiva_i2c1_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_TIVA_I2C2
static int tiva_i2c2_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_TIVA_I2C3
static int tiva_i2c3_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_TIVA_I2C4
static int tiva_i2c4_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_TIVA_I2C5
static int tiva_i2c5_interrupt(int irq, void *context);
#endif
#endif /* !CONFIG_I2C_POLLED */

static int tiva_i2c_initialize(struct tiva_i2c_priv_s *priv);
static int tiva_i2c_uninitialize(struct tiva_i2c_priv_s *priv);
static uint32_t tiva_i2c_setclock(struct tiva_i2c_priv_s *priv,
                                  uint32_t frequency);
static uint32_t tiva_i2c_setfrequency(struct i2c_dev_s *dev,
                                      uint32_t frequency);
static int tiva_i2c_setaddress(struct i2c_dev_s *dev, int addr, int nbits);
static int tiva_i2c_process(struct i2c_dev_s *dev, struct i2c_msg_s *msgs,
                            int count);
static int tiva_i2c_write(struct i2c_dev_s *dev, const uint8_t *buffer,
                          int buflen);
static int tiva_i2c_read(struct i2c_dev_s *dev, uint8_t *buffer, int buflen);

#ifdef CONFIG_I2C_WRITEREAD
static int tiva_i2c_writeread(struct i2c_dev_s *dev,
                              const uint8_t *wbuffer, int wbuflen,
                              uint8_t *buffer, int buflen);
#endif /* CONFIG_I2C_WRITEREAD */

#ifdef CONFIG_I2C_TRANSFER
static int tiva_i2c_transfer(struct i2c_dev_s *dev, struct i2c_msg_s *msgs,
                             int count);
#endif /* CONFIG_I2C_TRANSFER */

/************************************************************************************
 * Private Data
 ************************************************************************************/

#ifdef CONFIG_TIVA_I2C0
static const struct tiva_i2c_config_s tiva_i2c0_config =
{
  .base       = TIVA_I2C0_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C0,
#endif
  .scl_pin    = GPIO_I2C0_SCL,
  .sda_pin    = GPIO_I2C0_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c0_interrupt,
  .irq        = TIVA_IRQ_I2C0,
#endif
  .devno      = 0,
};

static struct tiva_i2c_priv_s tiva_i2c0_priv;
#endif

#ifdef CONFIG_TIVA_I2C1
static const struct tiva_i2c_config_s tiva_i2c1_config =
{
  .base       = TIVA_I2C1_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C1,
#endif
  .scl_pin    = GPIO_I2C1_SCL,
  .sda_pin    = GPIO_I2C1_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c1_interrupt,
  .irq        = TIVA_IRQ_I2C1,
#endif
  .devno      = 1,
};

static struct tiva_i2c_priv_s tiva_i2c1_priv;
#endif

#ifdef CONFIG_TIVA_I2C2
static const struct tiva_i2c_config_s tiva_i2c2_config =
{
  .base       = TIVA_I2C2_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C2,
#endif
  .scl_pin    = GPIO_I2C2_SCL,
  .sda_pin    = GPIO_I2C2_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c2_interrupt,
  .irq        = TIVA_IRQ_I2C2,
#endif
  .devno      = 2,
};

static struct tiva_i2c_priv_s tiva_i2c2_priv;
#endif

#ifdef CONFIG_TIVA_I2C3
static const struct tiva_i2c_config_s tiva_i2c3_config =
{
  .base       = TIVA_I2C3_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C3,
#endif
  .scl_pin    = GPIO_I2C3_SCL,
  .sda_pin    = GPIO_I2C3_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c3_interrupt,
  .irq        = TIVA_IRQ_I2C3,
#endif
  .devno      = 3,
};

static struct tiva_i2c_priv_s tiva_i2c3_priv;
#endif

#ifdef CONFIG_TIVA_I2C4
static const struct tiva_i2c_config_s tiva_i2c4_config =
{
  .base       = TIVA_I2C4_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C4,
#endif
  .scl_pin    = GPIO_I2C4_SCL,
  .sda_pin    = GPIO_I2C4_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c4_interrupt,
  .irq        = TIVA_IRQ_I2C4,
#endif
  .devno      = 4,
};

static struct tiva_i2c_priv_s tiva_i2c4_priv;
#endif

#ifdef CONFIG_TIVA_I2C5
static const struct tiva_i2c_config_s tiva_i2c5_config =
{
  .base       = TIVA_I2C5_BASE,
#ifndef TIVA_SYSCON_RCGCI2C
  .rcgbit     = SYSCON_RCGC1_I2C5,
#endif
  .scl_pin    = GPIO_I2C5_SCL,
  .sda_pin    = GPIO_I2C5_SDA,
#ifndef CONFIG_I2C_POLLED
  .isr        = tiva_i2c5_interrupt,
  .irq        = TIVA_IRQ_I2C5,
#endif
  .devno      = 5,
};

static struct tiva_i2c_priv_s tiva_i2c5_priv;
#endif

/* Device Structures, Instantiation */

static const struct i2c_ops_s tiva_i2c_ops =
{
  .setfrequency       = tiva_i2c_setfrequency,
  .setaddress         = tiva_i2c_setaddress,
  .write              = tiva_i2c_write,
  .read               = tiva_i2c_read
#ifdef CONFIG_I2C_WRITEREAD
  , .writeread        = tiva_i2c_writeread
#endif
#ifdef CONFIG_I2C_TRANSFER
  , .transfer         = tiva_i2c_transfer
#endif
#ifdef CONFIG_I2C_SLAVE
  , .setownaddress    = tiva_i2c_setownaddress,
    .registercallback = tiva_i2c_registercallback
#endif
};

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: tiva_i2c_getreg
 *
 * Description:
 *   Get a 16-bit register value by offset
 *
 ************************************************************************************/

static inline uint32_t tiva_i2c_getreg(struct tiva_i2c_priv_s *priv,
                                       unsigned int offset)
{
  return getreg32(priv->config->base + offset);
}

/************************************************************************************
 * Name: tiva_i2c_putreg
 *
 * Description:
 *  Put a 16-bit register value by offset
 *
 ************************************************************************************/

static inline void tiva_i2c_putreg(struct tiva_i2c_priv_s *priv,
                                   unsigned int offset, uint32_t regval)
{
  putreg32(regval, priv->config->base + offset);
}

/************************************************************************************
 * Name: tiva_i2c_modifyreg
 *
 * Description:
 *   Modify a 16-bit register value by offset
 *
 ************************************************************************************/

static inline void tiva_i2c_modifyreg(struct tiva_i2c_priv_s *priv,
                                      uint8_t offset, uint16_t clearbits,
                                      uint16_t setbits)
{
  modifyreg16(priv->config->base + offset, clearbits, setbits);
}

/************************************************************************************
 * Name: tiva_i2c_sem_wait
 *
 * Description:
 *   Take the exclusive access, waiting as necessary
 *
 ************************************************************************************/

static inline void tiva_i2c_sem_wait(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  while (sem_wait(&inst->priv->exclsem) != 0)
    {
      ASSERT(errno == EINTR);
    }
}

/************************************************************************************
 * Name: tiva_i2c_tousecs
 *
 * Description:
 *   Return a micro-second delay based on the number of bytes left to be processed.
 *
 ************************************************************************************/

#ifdef CONFIG_TIVA_I2C_DYNTIMEO
static useconds_t tiva_i2c_tousecs(int msgc, struct i2c_msg_s *msgs)
{
  size_t bytecount = 0;
  int i;

  /* Count the number of bytes left to process */

  for (i = 0; i < msgc; i++)
    {
      bytecount += msgs[i].length;
    }

  /* Then return a number of microseconds based on a user provided scaling
   * factor.
   */

  return (useconds_t)(CONFIG_TIVA_I2C_DYNTIMEO_USECPERBYTE * bytecount);
}
#endif

/************************************************************************************
 * Name: tiva_i2c_sem_waitdone
 *
 * Description:
 *   Wait for a transfer to complete
 *
 ************************************************************************************/

#ifndef CONFIG_I2C_POLLED
static inline int tiva_i2c_sem_waitdone(struct tiva_i2c_priv_s *priv)
{
  struct timespec abstime;
  irqstate_t flags;
  uint32_t regval;
  int ret;

  flags = irqsave();

  /* Enable the master interrupt. The I2C master module generates an interrupt when
   * a transaction completes (either transmit or receive), when arbitration is lost,
   * or when an error occurs during a transaction.
   */

  tiva_i2c_putreg(priv, TIVA_I2CM_IMR_OFFSET, I2CM_IMR_IM);

  /* Signal the interrupt handler that we are waiting.  NOTE:  Interrupts
   * are currently disabled but will be temporarily re-enabled below when
   * sem_timedwait() sleeps.
   */

  priv->intstate = INTSTATE_WAITING;
  do
    {
      /* Get the current time */

      (void)clock_gettime(CLOCK_REALTIME, &abstime);

      /* Calculate a time in the future */

#if CONFIG_TIVA_I2CTIMEOSEC > 0
      abstime.tv_sec += CONFIG_TIVA_I2CTIMEOSEC;
#endif

      /* Add a value proportional to the number of bytes in the transfer */

#ifdef CONFIG_TIVA_I2C_DYNTIMEO
      abstime.tv_nsec += 1000 * tiva_i2c_tousecs(priv->msgc, priv->msgv);
      if (abstime.tv_nsec > 1000 * 1000 * 1000)
        {
          abstime.tv_sec++;
          abstime.tv_nsec -= 1000 * 1000 * 1000;
        }

#elif CONFIG_TIVA_I2CTIMEOMS > 0
      abstime.tv_nsec += CONFIG_TIVA_I2CTIMEOMS * 1000 * 1000;
      if (abstime.tv_nsec > 1000 * 1000 * 1000)
        {
          abstime.tv_sec++;
          abstime.tv_nsec -= 1000 * 1000 * 1000;
        }
#endif

      /* Wait until either the transfer is complete or the timeout expires */

      ret = sem_timedwait(&priv->waitsem, &abstime);
      if (ret != OK && errno != EINTR)
        {
          /* Break out of the loop on irrecoverable errors.  This would
           * include timeouts and mystery errors reported by sem_timedwait.
           * NOTE that we try again if we are awakened by a signal (EINTR).
           */

          break;
        }
    }

  /* Loop until the interrupt level transfer is complete. */

  while (priv->intstate != INTSTATE_DONE);

  /* Set the interrupt state back to IDLE */

  priv->intstate = INTSTATE_IDLE;

  /* Disable I2C interrupts */

  tiva_i2c_putreg(priv, TIVA_I2CM_IMR_OFFSET, 0);

  irqrestore(flags);
  return ret;
}
#else
static inline int tiva_i2c_sem_waitdone(struct tiva_i2c_priv_s *priv)
{
  uint32_t timeout;
  uint32_t start;
  uint32_t elapsed;
  uint32_t status;
  int ret;

  /* Get the timeout value */

#ifdef CONFIG_TIVA_I2C_DYNTIMEO
  timeout = USEC2TICK(tiva_i2c_tousecs(priv->msgc, priv->msgv));
#else
  timeout = CONFIG_TIVA_I2CTIMEOTICKS;
#endif

  /* Signal the interrupt handler that we are waiting.  NOTE:  Interrupts
   * are currently disabled but will be temporarily re-enabled below when
   * sem_timedwait() sleeps.
   */

  priv->intstate = INTSTATE_WAITING;
  start = clock_systimer();

  do
    {
      /* Read the raw interrupt status */

      status = tiva_i2c_getreg(priv, TIVA_I2CM_RIS_OFFSET);

      /* Poll by simply calling the timer interrupt handler with the raw
       * interrupt status until it reports that it is done.
       */

      tiva_i2c_interrupt(priv, status);

      /* Calculate the elapsed time */

      elapsed = clock_systimer() - start;
    }

  /* Loop until the transfer is complete. */

  while (priv->intstate != INTSTATE_DONE && elapsed < timeout);

  i2cvdbg("intstate: %d elapsed: %d threshold: %d status: %08x\n",
          priv->intstate, elapsed, timeout, status);

  /* Set the interrupt state back to IDLE */

  ret = priv->intstate == INTSTATE_DONE ? OK : -ETIMEDOUT;
  priv->intstate = INTSTATE_IDLE;
  return ret;
}
#endif

/************************************************************************************
 * Name: tiva_i2c_sem_waitstop
 *
 * Description:
 *   Wait for a STOP to complete
 *
 ************************************************************************************/

static inline void tiva_i2c_sem_waitstop(struct tiva_i2c_priv_s *priv)
{
  uint32_t start;
  uint32_t elapsed;
  uint32_t timeout;

  /* Select a timeout */

#ifdef CONFIG_TIVA_I2C_DYNTIMEO
  timeout = USEC2TICK(CONFIG_TIVA_I2C_DYNTIMEO_STARTSTOP);
#else
  timeout = CONFIG_TIVA_I2CTIMEOTICKS;
#endif

  /* Wait as stop might still be in progress; but stop might also
   * be set because of a timeout error: "The [STOP] bit is set and
   * cleared by software, cleared by hardware when a Stop condition is
   * detected, set by hardware when a timeout error is detected."
   */

  start = clock_systimer();
  do
    {
      /* Check for STOP condition */
#warning Missing logic

      /* Check for timeout error */
#warning Missing logic


      /* Calculate the elapsed time */

      elapsed = clock_systimer() - start;
    }

  /* Loop until the stop is complete or a timeout occurs. */

  while (elapsed < timeout);

  /* If we get here then a timeout occurred with the STOP condition
   * still pending.
   */

  i2cvdbg("Timeout with CR1: %04x SR1: %04x\n", cr1, sr1);
}

/************************************************************************************
 * Name: tiva_i2c_sem_post
 *
 * Description:
 *   Release the mutual exclusion semaphore
 *
 ************************************************************************************/

static inline void tiva_i2c_sem_post(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  sem_post(&inst->priv->exclsem);
}

/************************************************************************************
 * Name: tiva_i2c_sem_init
 *
 * Description:
 *   Initialize semaphores
 *
 ************************************************************************************/

static inline void tiva_i2c_sem_init(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  sem_init(&inst->priv->exclsem, 0, 1);
#ifndef CONFIG_I2C_POLLED
  sem_init(&inst->priv->waitsem, 0, 0);
#endif
}

/************************************************************************************
 * Name: tiva_i2c_sem_destroy
 *
 * Description:
 *   Destroy semaphores.
 *
 ************************************************************************************/

static inline void tiva_i2c_sem_destroy(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  sem_destroy(&inst->priv->exclsem);
#ifndef CONFIG_I2C_POLLED
  sem_destroy(&inst->priv->waitsem);
#endif
}

/************************************************************************************
 * Name: tiva_i2c_trace*
 *
 * Description:
 *   I2C trace instrumentation
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_TRACE
static void tiva_i2c_traceclear(struct tiva_i2c_priv_s *priv)
{
  struct tiva_trace_s *trace = &priv->trace[priv->tndx];

  trace->status = 0;              /* I2C 32-bit SR2|SR1 status */
  trace->count  = 0;              /* Interrupt count when status change */
  trace->event  = I2CEVENT_NONE;  /* Last event that occurred with this status */
  trace->parm   = 0;              /* Parameter associated with the event */
  trace->time   = 0;              /* Time of first status or event */
}

static void tiva_i2c_tracereset(struct tiva_i2c_priv_s *priv)
{
  /* Reset the trace info for a new data collection */

  priv->tndx       = 0;
  priv->start_time = clock_systimer();
  tiva_i2c_traceclear(priv);
}

static void tiva_i2c_tracenew(struct tiva_i2c_priv_s *priv, uint32_t status)
{
  struct tiva_trace_s *trace = &priv->trace[priv->tndx];

  /* Is the current entry uninitialized?  Has the status changed? */

  if (trace->count == 0 || status != trace->status)
    {
      /* Yes.. Was it the status changed?  */

      if (trace->count != 0)
        {
          /* Yes.. bump up the trace index (unless we are out of trace entries) */

          if (priv->tndx >= (CONFIG_I2C_NTRACE-1))
            {
              i2cdbg("Trace table overflow\n");
              return;
            }

          priv->tndx++;
          trace = &priv->trace[priv->tndx];
        }

      /* Initialize the new trace entry */

      tiva_i2c_traceclear(priv);
      trace->status = status;
      trace->count  = 1;
      trace->time   = clock_systimer();
    }
  else
    {
      /* Just increment the count of times that we have seen this status */

      trace->count++;
    }
}

static void tiva_i2c_traceevent(struct tiva_i2c_priv_s *priv,
                                 enum tiva_trace_e event, uint32_t parm)
{
  struct tiva_trace_s *trace;

  if (event != I2CEVENT_NONE)
    {
      trace = &priv->trace[priv->tndx];

      /* Initialize the new trace entry */

      trace->event  = event;
      trace->parm   = parm;

      /* Bump up the trace index (unless we are out of trace entries) */

      if (priv->tndx >= (CONFIG_I2C_NTRACE-1))
        {
          i2cdbg("Trace table overflow\n");
          return;
        }

      priv->tndx++;
      tiva_i2c_traceclear(priv);
    }
}

static void tiva_i2c_tracedump(struct tiva_i2c_priv_s *priv)
{
  struct tiva_trace_s *trace;
  int i;

  syslog(LOG_DEBUG, "Elapsed time: %d\n",
         clock_systimer() - priv->start_time);

  for (i = 0; i <= priv->tndx; i++)
    {
      trace = &priv->trace[i];
      syslog(LOG_DEBUG,
             "%2d. STATUS: %08x COUNT: %3d EVENT: %2d PARM: %08x TIME: %d\n",
             i+1, trace->status, trace->count,  trace->event, trace->parm,
             trace->time - priv->start_time);
    }
}
#endif /* CONFIG_I2C_TRACE */

/************************************************************************************
 * Name: tiva_i2c_sendstart
 *
 * Description:
 *   Send the START conditions/force Master mode
 *
 ************************************************************************************/

static inline void tiva_i2c_sendstart(struct tiva_i2c_priv_s *priv)
{
  /* Disable ACK on receive by default and generate START */
#warning Missing logic
}

/************************************************************************************
 * Name: tiva_i2c_clrstart
 *
 * Description:
 *   Clear the STOP, START or PEC condition on certain error recovery steps.
 *
 ************************************************************************************/

static inline void tiva_i2c_clrstart(struct tiva_i2c_priv_s *priv)
{
#warning Missing logic
}

/************************************************************************************
 * Name: tiva_i2c_sendstop
 *
 * Description:
 *   Send the STOP conditions
 *
 ************************************************************************************/

static inline void tiva_i2c_sendstop(struct tiva_i2c_priv_s *priv)
{
#warning Missing logic
}

/************************************************************************************
 * Name: tiva_i2c_interrupt
 *
 * Description:
 *  Common Interrupt Service Routine
 *
 ************************************************************************************/

static int tiva_i2c_interrupt(struct tiva_i2c_priv_s *priv, uint32_t status)
{
  /* Check for new trace setup */

  tiva_i2c_tracenew(priv, status);

  /* Was start bit sent */
#warning Missing logic

    {
      tiva_i2c_traceevent(priv, I2CEVENT_SENDADDR, priv->msgc);

      /* We check for msgc > 0 here as an unexpected interrupt with
       * due to noise on the I2C cable can otherwise cause msgc to
       * wrap causing memory overwrite
       */

      if (priv->msgc > 0 && priv->msgv != NULL)
        {
          /* Get run-time data */

          priv->ptr   = priv->msgv->buffer;
          priv->dcnt  = priv->msgv->length;
          priv->flags = priv->msgv->flags;

          /* Send address byte and define addressing mode */

          tiva_i2c_putreg(priv, TIVA_I2CM_SA_OFFSET,
                          (priv->msgv->addr << 1) | (priv->flags & I2C_M_READ));

          /* Set ACK for receive mode */
#warning Missing logic

          /* Increment to next pointer and decrement message count */

          priv->msgv++;
          priv->msgc--;
        }
      else
        {
          /* Clear ISR by writing to DR register */
#warning Missing logic
        }
    }

  /* Was address sent, continue with either sending or reading data */
#warning Missing logic

  else if ((priv->flags & I2C_M_READ) == 0 && 0 /* ? */ )
    {
      if (priv->dcnt > 0)
        {
          /* Send a byte */
#warning Missing logic
          priv->dcnt--;
        }
    }

  else if ((priv->flags & I2C_M_READ) != 0 && 0 /* ? */)
    {
      /* Enable in order to receive one or multiple bytes */
#warning Missing logic
    }

  /* More bytes to read */
#warning Missing logic

    {
      /* Read a byte, if dcnt goes < 0, then read dummy bytes to ack ISRs */

      if (priv->dcnt > 0)
        {
          tiva_i2c_traceevent(priv, I2CEVENT_RCVBYTE, priv->dcnt);

          /* No interrupts or context switches may occur in the following
           * sequence.  Otherwise, additional bytes may be sent by the
           * device.
           */

#ifdef CONFIG_I2C_POLLED
          irqstate_t state = irqsave();
#endif
          /* Receive a byte */
#warning Missing logic

          /* Disable acknowledge when last byte is to be received */

          priv->dcnt--;
          if (priv->dcnt == 1)
            {
#warning Missing logic
            }

#ifdef CONFIG_I2C_POLLED
          irqrestore(state);
#endif
        }
      else
        {
          /* Throw away the unexpected byte */
#warning Missing logic
        }
    }

  /* Do we have more bytes to send, enable/disable buffer interrupts
   * (these ISRs could be replaced by DMAs)
   */

#ifndef CONFIG_I2C_POLLED
  if (priv->dcnt > 0)
    {
      tiva_i2c_traceevent(priv, I2CEVENT_REITBUFEN, 0);
#warning Missing logic
    }
  else if (priv->dcnt == 0)
    {
      tiva_i2c_traceevent(priv, I2CEVENT_DISITBUFEN, 0);
#warning Missing logic
    }
#endif

  /* Was last byte received or sent? */

  if (priv->dcnt <= 0 && 0 /* ? */)
    {
      /* Acknowledge the pending interrupt */
#warning Missing logic

      /* Do we need to terminate or restart after this byte?
       * If there are more messages to send, then we may:
       *
       *  - continue with repeated start
       *  - or just continue sending writeable part
       *  - or we close down by sending the stop bit
       */

      if (priv->msgc > 0)
        {
          if (priv->msgv->flags & I2C_M_NORESTART)
            {
              tiva_i2c_traceevent(priv, I2CEVENT_BTFNOSTART, priv->msgc);
              priv->ptr   = priv->msgv->buffer;
              priv->dcnt  = priv->msgv->length;
              priv->flags = priv->msgv->flags;
              priv->msgv++;
              priv->msgc--;

              /* Restart this ISR! */

#ifndef CONFIG_I2C_POLLED
#warning Missing logic
#endif
            }
          else
            {
              tiva_i2c_traceevent(priv, I2CEVENT_BTFRESTART, priv->msgc);
              tiva_i2c_sendstart(priv);
            }
        }
      else if (priv->msgv)
        {
          tiva_i2c_traceevent(priv, I2CEVENT_BTFSTOP, 0);
          tiva_i2c_sendstop(priv);

          /* Is there a thread waiting for this event (there should be) */

#ifndef CONFIG_I2C_POLLED
          if (priv->intstate == INTSTATE_WAITING)
            {
              /* Yes.. inform the thread that the transfer is complete
               * and wake it up.
               */

              sem_post(&priv->waitsem);
              priv->intstate = INTSTATE_DONE;
            }
#else
          priv->intstate = INTSTATE_DONE;
#endif

          /* Mark that we have stopped with this transaction */

          priv->msgv = NULL;
        }
    }

  /* Check for errors, in which case, stop the transfer and return
   * Note that in master reception mode AF becomes set on last byte
   * since ACK is not returned. We should ignore this error.
   */
#warning Missing logic

    {
      tiva_i2c_traceevent(priv, I2CEVENT_ERROR, 0);

      /* Clear interrupt flags */
#warning Missing logic


      /* Is there a thread waiting for this event (there should be) */

#ifndef CONFIG_I2C_POLLED
      if (priv->intstate == INTSTATE_WAITING)
        {
          /* Yes.. inform the thread that the transfer is complete
           * and wake it up.
           */

          sem_post(&priv->waitsem);
          priv->intstate = INTSTATE_DONE;
        }
#else
      priv->intstate = INTSTATE_DONE;
#endif
    }

  return OK;
}

/************************************************************************************
 * Name: tiva_i2c0_interrupt
 *
 * Description:
 *   I2C0 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C0)
static int tiva_i2c0_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c0_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c1_interrupt
 *
 * Description:
 *   I2C1 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C1)
static int tiva_i2c1_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c1_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c2_interrupt
 *
 * Description:
 *   I2C2 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C2)
static int tiva_i2c2_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c2_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c3_interrupt
 *
 * Description:
 *   I2C2 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C3)
static int tiva_i2c3_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c3_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c4_interrupt
 *
 * Description:
 *   I2C4 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C4)
static int tiva_i2c4_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c4_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c5_interrupt
 *
 * Description:
 *   I2C5 interrupt service routine
 *
 ************************************************************************************/

#if !defined(CONFIG_I2C_POLLED) && defined(CONFIG_TIVA_I2C5)
static int tiva_i2c5_interrupt(int irq, void *context)
{
  struct tiva_i2c_priv_s *priv;
  uint32_t status;

  /* Read the masked interrupt status */

  priv   = &tiva_i2c5_priv;
  status = tiva_i2c_getreg(priv, TIVA_I2CM_MIS_OFFSET);

  /* Let the common interrupt handler do the rest of the work */

  return tiva_i2c_interrupt(priv, status);
}
#endif

/************************************************************************************
 * Name: tiva_i2c_initialize
 *
 * Description:
 *   Setup the I2C hardware, ready for operation with defaults
 *
 ************************************************************************************/

static int tiva_i2c_initialize(struct tiva_i2c_priv_s *priv)
{
  uint32_t regval;
  int ret;

  /* Enable clocking to the I2C peripheral */

#ifdef TIVA_SYSCON_RCGCI2C
  modifyreg32(TIVA_SYSCON_RCGCI2C, 0, SYSCON_RCGCI2C(priv->config->devno));
#else
  modifyreg32(TIVA_SYSCON_RCGC1, 0, priv->rcgbit);
#endif

  /* Configure pins */

  ret = tiva_configgpio(priv->config->scl_pin);
  if (ret < 0)
    {
      return ret;
    }

  ret = tiva_configgpio(priv->config->sda_pin);
  if (ret < 0)
    {
      tiva_configgpio(MKI2C_INPUT(priv->config->scl_pin));
      return ret;
    }

  /* Enable the I2C master block */

  regval = tiva_i2c_getreg(priv, TIVA_I2CM_CR_OFFSET);
  regval |= I2CM_CR_MFE;
  tiva_i2c_putreg(priv, TIVA_I2CM_CR_OFFSET, regval);

  /* Configure the the initial I2C clock frequency. */

  (void)tiva_i2c_setclock(priv, 100000);

  /* Attach interrupt handlers and enable interrupts at the NVIC (still
   * disabled at the source).
   */

#ifndef CONFIG_I2C_POLLED
  irq_attach(priv->config->irq, priv->config->isr);
  up_enable_irq(priv->config->irq);
#endif
  return OK;
}

/************************************************************************************
 * Name: tiva_i2c_uninitialize
 *
 * Description:
 *   Shutdown the I2C hardware
 *
 ************************************************************************************/

static int tiva_i2c_uninitialize(struct tiva_i2c_priv_s *priv)
{
  uint32_t regval;

  /* Disable I2C */

  regval  = tiva_i2c_getreg(priv, TIVA_I2CM_CR_OFFSET);
  regval &= ~I2CM_CR_MFE;
  tiva_i2c_putreg(priv, TIVA_I2CM_CR_OFFSET, regval);

  /* Unconfigure GPIO pins */

  tiva_configgpio(MKI2C_INPUT(priv->config->scl_pin));
  tiva_configgpio(MKI2C_INPUT(priv->config->sda_pin));

  /* Disable and detach interrupts */

#ifndef CONFIG_I2C_POLLED
  up_disable_irq(priv->config->irq);
  irq_detach(priv->config->irq);
#endif

  /* Disable clocking */

#ifdef TIVA_SYSCON_RCGCI2C
  modifyreg32(TIVA_SYSCON_RCGCI2C, SYSCON_RCGCI2C(priv->config->devno), 0);
#else
  modifyreg32(TIVA_SYSCON_RCGC1, priv->rcgbit, 0);
#endif

  return OK;
}

/************************************************************************************
 * Name: tiva_i2c_setclock
 *
 * Description:
 *   Set the I2C frequency
 *
 ************************************************************************************/

static uint32_t tiva_i2c_setclock(struct tiva_i2c_priv_s *priv, uint32_t frequency)
{
  uint32_t regval;
  uint32_t tmp;

  /* Calculate the clock divider that results in the highest frequency that
   * is than or equal to the desired speed.
   */

  tmp = 2 * 10 * frequency;
  regval = (((SYSCLK_FREQUENCY + tmp - 1) / tmp) - 1) << I2CM_TPR_SHIFT;

  DEBUGASSERT((regval & I2CM_TPR_MASK) == 0);
  tiva_i2c_putreg(priv, TIVA_I2CM_TPR_OFFSET, regval);

#ifdef TIVA_I2CSC_PP_OFFSET
  /* If the I2C peripheral is High-Speed enabled then choose the highest
   * speed that is less than or equal to 3.4 Mbps.
   */

  regval = tiva_i2c_putreg(priv, TIVA_I2CSC_PP_OFFSET);
  if ((regval & I2CSC_PP_HS) != 0)
    {
      regval = ((SYSCLK_FREQUENCY +
                (2 * 3 * 3400000) - 1) / (2 * 3 * 3400000)) - 1;

      tiva_i2c_putreg(priv, TIVA_I2CM_TPR_OFFSET,  I2CM_TPR_HS | regval);
    }
#endif

  return frequency;
}
/************************************************************************************
 * Name: tiva_i2c_setfrequency
 *
 * Description:
 *   Set the I2C frequency
 *
 ************************************************************************************/

static uint32_t tiva_i2c_setfrequency(struct i2c_dev_s *dev, uint32_t frequency)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  struct tiva_i2c_priv_s *priv;

  DEBUGASSERT(inst && inst->priv);
  priv = inst->priv;

  /* Get exclusive access to the I2C device */

  tiva_i2c_sem_wait(dev);

  /* Set the clocking for the selected frequency */

  inst->frequency = tiva_i2c_setclock(priv, frequency);

  tiva_i2c_sem_post(dev);
  return frequency;
}

/************************************************************************************
 * Name: tiva_i2c_setaddress
 *
 * Description:
 *   Set the I2C slave address
 *
 ************************************************************************************/

static int tiva_i2c_setaddress(struct i2c_dev_s *dev, int addr, int nbits)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  tiva_i2c_sem_wait(dev);

  inst->address = addr;
  inst->flags   = (nbits == 10) ? I2C_M_TEN : 0;

  tiva_i2c_sem_post(dev);
  return OK;
}

/************************************************************************************
 * Name: tiva_i2c_process
 *
 * Description:
 *   Common I2C transfer logic
 *
 ************************************************************************************/

static int tiva_i2c_process(struct i2c_dev_s *dev, struct i2c_msg_s *msgs,
                             int count)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  struct tiva_i2c_priv_s *priv = inst->priv;
  uint32_t status = 0;
  int errval = 0;

  ASSERT(count);

  /* Wait for any STOP in progress.  NOTE:  If we have to disable the FSMC
   * then we cannot do this at the top of the loop, unfortunately.  The STOP
   * will not complete normally if the FSMC is enabled.
   */

  tiva_i2c_sem_waitstop(priv);

  /* Clear any pending error interrupts */
#warning Missing logic

  /* "Note: When the STOP, START or PEC bit is set, the software must
   *  not perform any write access to I2C_CR1 before this bit is
   *  cleared by hardware. Otherwise there is a risk of setting a
   *  second STOP, START or PEC request."  However, if the bits are
   *  not cleared by hardware, then we will have to do that from hardware.
   */

  tiva_i2c_clrstart(priv);

  /* Old transfers are done */

  /* Reset ptr and dcnt to ensure an unexpected data interrupt doesn't
   * overwrite stale data.
   */

  priv->dcnt = 0;
  priv->ptr = NULL;

  priv->msgv = msgs;
  priv->msgc = count;

  /* Reset I2C trace logic */

  tiva_i2c_tracereset(priv);

  /* Set I2C clock frequency  */

  tiva_i2c_setclock(priv, inst->frequency);

  /* Trigger start condition, then the process moves into the ISR.  I2C
   * interrupts will be enabled within tiva_i2c_waitdone().
   */

  tiva_i2c_sendstart(priv);

  /* Wait for an ISR, if there was a timeout, fetch latest status to get
   * the BUSY flag.
   */

  if (tiva_i2c_sem_waitdone(priv) < 0)
    {
      /* Read the raw interrupt status */

      status = tiva_i2c_getreg(priv, TIVA_I2CM_RIS_OFFSET);
      errval = ETIMEDOUT;

      i2cdbg("Timed out: status: 0x%08x\n", status);

      /* "Note: When the STOP, START or PEC bit is set, the software must
       *  not perform any write access to I2C_CR1 before this bit is
       *  cleared by hardware. Otherwise there is a risk of setting a
       *  second STOP, START or PEC request."
       */

      tiva_i2c_clrstart(priv);
    }

  /* Check for error status conditions */
#warning Missing logic
    {
        {
          /* Bus Error */

          errval = EIO;
        }
        {
          /* Arbitration Lost (master mode) */

          errval = EAGAIN;
        }
        {
          /* Acknowledge Failure */

          errval = ENXIO;
        }
        {
          /* Overrun/Underrun */

          errval = EIO;
        }
        {
          /* PEC Error in reception */

          errval = EPROTO;
        }
        {
          /* Timeout or Tlow Error */

          errval = ETIME;
        }

      /* This is not an error and should never happen since SMBus is not enabled */

        {
          /* SMBus alert is an optional signal with an interrupt line for devices
           * that want to trade their ability to master for a pin.
           */

          errval = EINTR;
        }
    }

  /* This is not an error, but should not happen.  The BUSY signal can hang,
   * however, if there are unhealthy devices on the bus that need to be reset.
   * NOTE:  We will only see this busy indication if tiva_i2c_sem_waitdone()
   * fails above;  Otherwise it is cleared.
   */
#warning Missing logic
    {
      /* I2C Bus is for some reason busy */

      errval = EBUSY;
    }

  /* Dump the trace result */

  tiva_i2c_tracedump(priv);

  /* Ensure that any ISR happening after we finish can't overwrite any user data */

  priv->dcnt = 0;
  priv->ptr = NULL;

  tiva_i2c_sem_post(dev);

  return -errval;
}

/************************************************************************************
 * Name: tiva_i2c_write
 *
 * Description:
 *   Write I2C data
 *
 ************************************************************************************/

static int tiva_i2c_write(struct i2c_dev_s *dev, const uint8_t *buffer, int buflen)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  tiva_i2c_sem_wait(dev);   /* ensure that address or flags don't change meanwhile */

  struct i2c_msg_s msgv =
  {
    .addr   = inst->address,
    .flags  = inst->flags,
    .buffer = (uint8_t *)buffer,
    .length = buflen
  };

  return tiva_i2c_process(dev, &msgv, 1);
}

/************************************************************************************
 * Name: tiva_i2c_read
 *
 * Description:
 *   Read I2C data
 *
 ************************************************************************************/

int tiva_i2c_read(struct i2c_dev_s *dev, uint8_t *buffer, int buflen)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;

  tiva_i2c_sem_wait(dev);   /* ensure that address or flags don't change meanwhile */

  struct i2c_msg_s msgv =
  {
    .addr   = inst->address,
    .flags  = inst->flags | I2C_M_READ,
    .buffer = buffer,
    .length = buflen
  };

  return tiva_i2c_process(dev, &msgv, 1);
}

/************************************************************************************
 * Name: tiva_i2c_writeread
 *
 * Description:
 *  Read then write I2C data
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_WRITEREAD
static int tiva_i2c_writeread(struct i2c_dev_s *dev,
                              const uint8_t *wbuffer, int wbuflen,
                              uint8_t *buffer, int buflen)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  tiva_i2c_sem_wait(dev);   /* Ensure that address or flags don't change meanwhile */

  struct i2c_msg_s msgv[2] =
  {
    {
      .addr   = inst->address,
      .flags  = inst->flags,
      .buffer = (uint8_t *)wbuffer,          /* This is really ugly, sorry const ... */
      .length = wbuflen
    },
    {
      .addr   = inst->address,
      .flags  = inst->flags | ((buflen>0) ? I2C_M_READ : I2C_M_NORESTART),
      .buffer = buffer,
      .length = (buflen>0) ? buflen : -buflen
    }
  };

  return tiva_i2c_process(dev, msgv, 2);
}
#endif

/************************************************************************************
 * Name: tiva_i2c_transfer
 *
 * Description:
 *   Generic I2C transfer function
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_TRANSFER
static int tiva_i2c_transfer(struct i2c_dev_s *dev, struct i2c_msg_s *msgs,
                             int count)
{
  tiva_i2c_sem_wait(dev);   /* Ensure that address or flags don't change meanwhile */
  return tiva_i2c_process(dev, msgs, count);
}
#endif

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: up_i2cinitialize
 *
 * Description:
 *   Initialize one I2C bus
 *
 ************************************************************************************/

struct i2c_dev_s *up_i2cinitialize(int port)
{
  struct tiva_i2c_priv_s * priv = NULL;   /* Private data of device with multiple instances */
  struct tiva_i2c_inst_s * inst = NULL;   /* Device, single instance */
  const struct tiva_i2c_config_s *config; /* Constant configuration */
  int irqs;

  /* Get I2C private structure */

  switch (port)
    {
#ifdef CONFIG_TIVA_I2C0
    case 0:
      priv   = &tiva_i2c0_priv;
      config = &tiva_i2c0_config;
      break;
#endif
#ifdef CONFIG_TIVA_I2C1
    case 1:
      priv   = &tiva_i2c1_priv;
      config = &tiva_i2c1_config;
      break;
#endif
#ifdef CONFIG_TIVA_I2C2
    case 2:
      priv   = &tiva_i2c2_priv;
      config = &tiva_i2c2_config;
      break;
#endif
#ifdef CONFIG_TIVA_I2C3
    case 3:
      priv   = &tiva_i2c3_priv;
      config = &tiva_i2c3_config;
      break;
#endif
#ifdef CONFIG_TIVA_I2C4
    case 4:
      priv   = &tiva_i2c4_priv;
      config = &tiva_i2c4_config;
      break;
#endif
#ifdef CONFIG_TIVA_I2C5
    case 5:
      priv   = &tiva_i2c5_priv;
      config = &tiva_i2c5_config;
      break;
#endif
    default:
      return NULL;
    }

  /* Allocate instance */

  if (!(inst = kmm_malloc(sizeof(struct tiva_i2c_inst_s))))
    {
      return NULL;
    }

  /* Make sure that the device structure is inialized */

  priv->config    = config;

  /* Initialize instance */

  inst->ops       = &tiva_i2c_ops;
  inst->priv      = priv;
  inst->frequency = 100000;
  inst->address   = 0;
  inst->flags     = 0;

  /* Initialize private data for the first time, increment reference count,
   * power-up hardware and configure GPIOs.
   */

  irqs = irqsave();

  if ((volatile int)priv->refs++ == 0)
    {
      tiva_i2c_sem_init((struct i2c_dev_s *)inst);
      tiva_i2c_initialize(priv);
    }

  irqrestore(irqs);
  return (struct i2c_dev_s *)inst;
}

/************************************************************************************
 * Name: up_i2cuninitialize
 *
 * Description:
 *   Uninitialize an I2C bus
 *
 ************************************************************************************/

int up_i2cuninitialize(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  int irqs;

  ASSERT(dev);

  /* Decrement reference count and check for underflow */

  if (inst->priv->refs == 0)
    {
      return ERROR;
    }

  irqs = irqsave();

  if (--inst->priv->refs)
    {
      irqrestore(irqs);
      kmm_free(dev);
      return OK;
    }

  irqrestore(irqs);

  /* Disable power and other HW resource (GPIO's) */

  tiva_i2c_uninitialize(inst->priv);

  /* Release unused resources */

  tiva_i2c_sem_destroy((struct i2c_dev_s *)dev);

  kmm_free(dev);
  return OK;
}

/************************************************************************************
 * Name: up_i2creset
 *
 * Description:
 *   Reset an I2C bus
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_RESET
int up_i2creset(struct i2c_dev_s *dev)
{
  struct tiva_i2c_inst_s *inst = (struct tiva_i2c_inst_s *)dev;
  struct tiva_i2c_priv_s *priv;
  unsigned int clock_count;
  unsigned int stretch_count;
  uint32_t scl_gpio;
  uint32_t sda_gpio;
  int ret = ERROR;

  ASSERT(dev);

  /* Get I2C private structure */

  priv = inst->priv;

  /* Our caller must own a ref */

  ASSERT(priv->refs > 0);

  /* Lock out other clients */

  tiva_i2c_sem_wait(dev);

  /* Un-initialize the port */

  tiva_i2c_uninitialize(priv);

  /* Use GPIO configuration to un-wedge the bus */

  scl_gpio = MKI2C_OUTPUT(priv->config->scl_pin);
  sda_gpio = MKI2C_OUTPUT(priv->config->sda_pin);

  tiva_configgpio(scl_gpio);
  tiva_configgpio(sda_gpio);

  /* Let SDA go high */

  tiva_gpiowrite(sda_gpio, 1);

  /* Clock the bus until any slaves currently driving it let it go. */

  clock_count = 0;
  while (!tiva_gpioread(sda_gpio))
    {
      /* Give up if we have tried too hard */

      if (clock_count++ > 10)
        {
          goto out;
        }

      /* Sniff to make sure that clock stretching has finished.
       *
       * If the bus never relaxes, the reset has failed.
       */

      stretch_count = 0;
      while (!tiva_gpioread(scl_gpio))
        {
          /* Give up if we have tried too hard */

          if (stretch_count++ > 10)
            {
              goto out;
            }

          up_udelay(10);
        }

      /* Drive SCL low */

      tiva_gpiowrite(scl_gpio, 0);
      up_udelay(10);

      /* Drive SCL high again */

      tiva_gpiowrite(scl_gpio, 1);
      up_udelay(10);
    }

  /* Generate a start followed by a stop to reset slave
   * state machines.
   */

  tiva_gpiowrite(sda_gpio, 0);
  up_udelay(10);
  tiva_gpiowrite(scl_gpio, 0);
  up_udelay(10);
  tiva_gpiowrite(scl_gpio, 1);
  up_udelay(10);
  tiva_gpiowrite(sda_gpio, 1);
  up_udelay(10);

  /* Revert the GPIO configuration. */

  tiva_configgpio(MKI2C_INPUT(sda_gpio));
  tiva_configgpio(MKI2C_INPUT(scl_gpio));

  /* Re-init the port */

  tiva_i2c_initialize(priv);
  ret = OK;

out:

  /* Release the port for re-use by other clients */

  tiva_i2c_sem_post(dev);
  return ret;
}
#endif /* CONFIG_I2C_RESET */

#endif /* CONFIG_TIVA_I2C0 ... CONFIG_TIVA_I2C5 */