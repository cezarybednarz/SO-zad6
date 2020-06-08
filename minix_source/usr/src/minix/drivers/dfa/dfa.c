#include <minix/drivers.h>
#include <minix/chardriver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minix/ds.h>
#include <minix/ioctl.h>
#include <sys/ioc_dfa.h>


static ssize_t dfa_read(devminor_t minor, u64_t position, endpoint_t endpt,
    cp_grant_id_t grant, size_t size, int flags, cdev_id_t id);
static int dfa_ioctl(devminor_t minor, unsigned long request, endpoint_t endpt,
    cp_grant_id_t grant, int flags, endpoint_t user_endpt, cdev_id_t id);

/* SEF functions and variables. */
static void sef_local_startup(void);
static int sef_cb_init(int type, sef_init_info_t *info);
static int sef_cb_lu_state_save(int);
static int lu_state_restore(void);

/* Entry points to the hello driver. */
static struct chardriver dfa_tab =
{
	.cdr_read	= dfa_read,
    .cdr_ioctl  = dfa_ioctl
};

/* State variable to save hello message. */
#define A_SIZE 256
static char automaton[A_SIZE*A_SIZE + 1];
static char accepting[A_SIZE + 1];
static char current_state = 0;

static ssize_t dfa_read(devminor_t UNUSED(minor), u64_t position,
    endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
    cdev_id_t UNUSED(id))
{
    u64_t dev_size;
    char *ptr;
    int ret;
    char answer = 'Y';
    if(!accepting[current_state])
    	answer = 'N';

    /* This is the total size of our device. */
    dev_size = (u64_t) 4;

    char buf[dev_size];
    for (size_t i = 0; i < dev_size; i++)
    	buf[i] = answer;

    printf("dfa_read()\n");


    /* Check for EOF, and possibly limit the read size. */
    if (position >= dev_size)
        return 0; /* EOF */
    if (position + size > dev_size)
        size = (size_t)(dev_size - position);	/* Limit size. */

    /* Copy the requested part to the caller. */
    ptr = buf + (size_t)position;
    if ((ret = sys_safecopyto(endpt, grant, 0, (vir_bytes) ptr, size)) != OK)
        return ret;

    /* Return the number of bytes read. */
    return size;
}

static int dfa_ioctl(devminor_t UNUSED(minor), unsigned long request, endpoint_t endpt,
    cp_grant_id_t grant, int UNUSED(flags), endpoint_t user_endpt, cdev_id_t UNUSED(id))
{
    int rc;
    char buf[3];

    switch(request) {
    case DFAIOCRESET:
    	current_state = 0; /* reset to state q_0 */
    	break;
    case DFAIOCADD:
    	rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 1);
		if (rc == OK) {
			int i = buf[0]; /* row */
			int j = buf[1]; /* column */
			automaton[i*A_SIZE + j] = buf[2];
		}
    	break;
    case DFAIOCACCEPT:
    	rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 1);
		if (rc == OK) {
			accepting[buf[0]] = 1; /* accept state */
		}
		break;
    case DFAIOCREJECT:
    	rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 1);
		if (rc == OK) {
			accepting[buf[0]] = 0; /* reject state */
		}
		break;
    default:
        rc = ENOTTY;
    }

    return rc;
}

static int sef_cb_lu_state_save(int UNUSED(state)) {
    /* Save the state. */
    ds_publish_u32("current_state", current_state, DSF_OVERWRITE);
    ds_publish_str("automaton", automaton, DSF_OVERWRITE);
    ds_publish_str("accepting", accepting, DSF_OVERWRITE);

    return OK;
}

static int lu_state_restore() {
    /* Restore the state. */
    u32_t value;

    ds_retrieve_u32("current_state", &value);
    ds_delete_u32("current_state");
    current_state = (int)value;

    ds_retrieve_str("automaton", automaton, A_SIZE*A_SIZE + 1);
    ds_delete_str("automaton");

    ds_retrieve_str("accepting", accepting, A_SIZE + 1);
    ds_delete_str("accepting");


    return OK;
}

static void sef_local_startup()
{
    /* Register init callbacks. Use the same function for all event types. */
    sef_setcb_init_fresh(sef_cb_init);
    sef_setcb_init_lu(sef_cb_init);
    sef_setcb_init_restart(sef_cb_init);

    /* Register live update callbacks. */
    /* - Agree to update immediately when LU is requested in a valid state. */
    sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
    /* - Support live update starting from any standard state. */
    sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
    /* - Register a custom routine to save the state. */
    sef_setcb_lu_state_save(sef_cb_lu_state_save);

    /* Let SEF perform startup. */
    sef_startup();
}

void init_arrays() {
	for (size_t i = 0; i < A_SIZE; i++) {
		accepting[i] = 0;
		for (size_t j = 0; j < A_SIZE; j++){
			automaton[i*A_SIZE + j] = 0;
		}
	}
	automaton[A_SIZE*A_SIZE] = 0; /* null termination */
	accepting[A_SIZE] = 0;        /* null termination */
	current_state = 0; /* state q_0 */
}

static int sef_cb_init(int type, sef_init_info_t *UNUSED(info))
{
    /* Initialize the hello driver. */
    int do_announce_driver = TRUE;


	init_arrays();

    switch(type) {
        case SEF_INIT_FRESH:
            init_arrays();
        break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;
            // strncpy(hello_msg, HELLO_MESSAGE, HELLO_LEN);
            // hello_msg[HELLO_LEN - 1] = 0;
            printf("[%d] Hey, I'm a new version!\n", current_state);
        break;

        case SEF_INIT_RESTART:
            // strncpy(hello_msg, HELLO_MESSAGE, HELLO_LEN);
            // hello_msg[HELLO_LEN - 1] = 0;
            printf("[%d] Hey, I've just been restarted!\n", current_state);
        break;
    }

    /* Announce we are up when necessary. */
    if (do_announce_driver) {
        chardriver_announce();
    }

    /* Initialization completed successfully. */
    return OK;
}

int main(void)
{
    /* Perform initialization. */
    sef_local_startup();

    /* Run the main loop. */
    chardriver_task(&dfa_tab);
    return OK;
}
