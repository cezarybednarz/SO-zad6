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
static ssize_t dfa_write(devminor_t minor, u64_t position, endpoint_t endpt,
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
	.cdr_write  = dfa_write,
    .cdr_ioctl  = dfa_ioctl
};

/* State variable to save hello message. */
#define A_SIZE 256
static char automaton[A_SIZE*A_SIZE + 1];
static char accepting[A_SIZE + 1];
static char current_state = 0;
static int initialized = 0;

static ssize_t dfa_read(devminor_t UNUSED(minor), u64_t position,
    endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
    cdev_id_t UNUSED(id))
{
    int ret;
    char answer = 'Y';
    if(!accepting[current_state])
    	answer = 'N';

    /* Copy the requested part to the caller. */
    if ((ret = sys_safememset(endpt, grant, 0, answer, size)) != OK)
    	return ret;

    /* Return the number of bytes read. */
    return size;
}

static ssize_t dfa_write(devminor_t UNUSED(minor), u64_t position,
	endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
	cdev_id_t UNUSED(id))
{
	char buf[UINT16_MAX];
	int ret;
	if ((ret = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, size)) != OK) {
		return ret;
	}

	for (size_t i = 0; i < size; i++) {
		current_state = automaton[current_state*A_SIZE + buf[i]];
	}

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
    	rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 3);
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
	ds_publish_u32("initialized", initialized, DSF_OVERWRITE);
    ds_publish_u32("current_state", current_state, DSF_OVERWRITE);
    ds_publish_str("automaton", automaton, DSF_OVERWRITE);
    ds_publish_str("accepting", accepting, DSF_OVERWRITE);

    return OK;
}

static int lu_state_restore() {
    /* Restore the state. */
    u32_t value;

    ds_retrieve_u32("initialized", &value);
    ds_delete_u32("initialized");
    initialized = (int)value;

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
	if (initialized) { /* already initialized */
		return;
	}
	initialized = 1;
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

    switch(type) {
        case SEF_INIT_FRESH:
            /* Restore the state. */
			lu_state_restore();
			do_announce_driver = FALSE;
			init_arrays();
        break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;
        	init_arrays();
        break;

        case SEF_INIT_RESTART:
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
