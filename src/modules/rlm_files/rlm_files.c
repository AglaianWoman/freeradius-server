/*
 * rlm_files.c	authorization: Find a user in the "users" file.
 *		accounting:    Write the "detail" files.
 *
 * Version:	$Id$
 *
 */

static const char rcsid[] = "$Id$";

#include	"autoconf.h"

#include	<sys/types.h>
#include	<sys/socket.h>
#include	<sys/time.h>
#include	<sys/stat.h>
#include	<netinet/in.h>

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<errno.h>
#include	<netdb.h>
#include	<pwd.h>
#include	<grp.h>
#include	<time.h>
#include	<ctype.h>
#include	<fcntl.h>
#include	<unistd.h>
#include        <limits.h>

#if HAVE_MALLOC_H
#  include	<malloc.h>
#endif

#include	"radiusd.h"
#include	"modules.h"

#ifdef WITH_DBM
#  include	<dbm.h>
#endif
#ifdef WITH_NDBM
#  include	<ndbm.h>
#endif

struct file_instance {
	char *compat_mode;

        /* autz */
        char *usersfile;
        PAIR_LIST *users;

        /* preacct */
        char *acctusersfile;
        PAIR_LIST *acctusers;

        /*acct*/
        int detailperm;
};

#if defined(WITH_DBM) || defined(WITH_NDBM)
/*
 *	See if a potential DBM file is present.
 */
static int checkdbm(char *users, char *ext)
{
	char buffer[256];
	struct stat st;

	strcpy(buffer, users);
	strcat(buffer, ext);

	return stat(buffer, &st);
}

/*
 *	Find the named user in the DBM user database.
 *	Returns: -1 not found
 *		  0 found but doesn't match.
 *		  1 found and matches.
 */
static int dbm_find(DBM *dbmfile, char *name, VALUE_PAIR *request_pairs,
		VALUE_PAIR **check_pairs, VALUE_PAIR **reply_pairs)
{
	datum		named;
	datum		contentd;
	char		*ptr;
	VALUE_PAIR	*check_tmp;
	VALUE_PAIR	*reply_tmp;
	int		ret = 0;

	named.dptr = name;
	named.dsize = strlen(name);
#ifdef WITH_DBM
	contentd = fetch(named);
#endif
#ifdef WITH_NDBM
	contentd = dbm_fetch(dbmfile, named);
#endif
	if(contentd.dptr == NULL)
		return -1;

	check_tmp = NULL;
	reply_tmp = NULL;

	/*
	 *	Parse the check values
	 */
	ptr = contentd.dptr;
	contentd.dptr[contentd.dsize] = '\0';

	if (*ptr != '\n' && userparse(ptr, &check_tmp) != 0) {
		log(L_ERR|L_CONS, "Parse error (check) for user %s", name);
		pairfree(check_tmp);
		return -1;
	}
	while(*ptr != '\n' && *ptr != '\0') {
		ptr++;
	}
	if(*ptr != '\n') {
		log(L_ERR|L_CONS, "Parse error (no reply pairs) for user %s",
			name);
		pairfree(check_tmp);
		return -1;
	}
	ptr++;

	/*
	 *	Parse the reply values
	 */
	if (userparse(ptr, &reply_tmp) != 0) {
		log(L_ERR|L_CONS, "Parse error (reply) for user %s", name);
		pairfree(check_tmp);
		pairfree(reply_tmp);
		return -1;
	}

	/*
	 *	See if the check_pairs match.
	 */
	if (paircmp(request_pairs, check_tmp, reply_pairs) == 0) {
		ret = 1;
		pairmove(reply_pairs, &reply_tmp);
		pairmove2(reply_pairs, &reply_tmp, PW_FALL_THROUGH);
		pairmove(check_pairs, &check_tmp);
	}
	pairfree(reply_tmp);
	pairfree(check_tmp);

	return ret;
}
#endif /* DBM */

/*
 *     See if a VALUE_PAIR list contains Fall-Through = Yes
 */
static int fallthrough(VALUE_PAIR *vp)
{
	VALUE_PAIR *tmp;

	tmp = pairfind(vp, PW_FALL_THROUGH);

	return tmp ? tmp->lvalue : 0;
}

#define DL_FLAG_START	  1
#define DL_FLAG_STOP	  2
#define DL_FLAG_ACCT_ON   4
#define DL_FLAG_ACCT_OFF  8
#define DL_FLAG_ALIVE	 16

typedef struct dyn_log {
	char dir[256];
	char fname[256];
	char fmt[1024];
	char mode[5];
	int flags;
} DYN_LOG;
#define MAX_LOGS 20
static DYN_LOG logcfg[MAX_LOGS];
static int logcnt;

/*
 * Initialize dynamic logging
 */
static void file_getline(FILE *f,char * buff,int len)
{
	char tmp[2048];
	int i;

	tmp[0] = '\0';
	while (!feof(f)) {
		fgets(tmp,len,f);
		if (tmp[0] != '#') {
			break;
		}
	}
	i = 0;
	while (tmp[i] != '\n') {
		*buff = tmp[i];
		buff++;
		i++;
	}
}

static void file_dynamic_log_init(void)
{
	FILE * f;
	char fn[1024];

	sprintf(fn,"%s/%s",radius_dir,"rlm_files_log.cfg");
	logcnt = 0;
	f = fopen(fn, "r");
	if (f != NULL) {
		log_debug("Loading %s",fn);
		while (logcnt < MAX_LOGS) {
			file_getline(f,logcfg[logcnt].dir,sizeof(logcfg[logcnt].dir));
			file_getline(f,logcfg[logcnt].fname,sizeof(logcfg[logcnt].fname));
			file_getline(f,logcfg[logcnt].fmt,sizeof(logcfg[logcnt].fmt));
			file_getline(f,logcfg[logcnt].mode,sizeof(logcfg[logcnt].mode));
			file_getline(f,fn,sizeof(fn));
			logcfg[logcnt].flags = atoi(fn);
			if ((logcfg[logcnt].flags != 0) &&
			    (strlen(logcfg[logcnt].mode) != 0)) {
				logcnt++;
			} else {
				break;
			}
		}
		log_debug("%d logs configured",logcnt);
		fclose(f);
	} else {
		log_debug("Error loading %s: %s",fn, strerror(errno));
	}


}


static int file_init(void)
{
	file_dynamic_log_init();

	return 0;
}


/*
 *	A temporary holding area for config values to be extracted
 *	into, before they are copied into the instance data
 */
static struct file_instance config;

static CONF_PARSER module_config[] = {
        { "usersfile",     PW_TYPE_STRING_PTR, &config.usersfile, RADIUS_USERS },
        { "acctusersfile", PW_TYPE_STRING_PTR, &config.acctusersfile, RADIUS_ACCT_USERS },
        { "detailperm",    PW_TYPE_INTEGER,    &config.detailperm, "0600" },
	{ "compat",        PW_TYPE_STRING_PTR, &config.compat_mode, "cistron" },
	{ NULL, -1, NULL, NULL }
};

static int getusersfile(const char *filename, PAIR_LIST **pair_list)
{
	int rcode;
        PAIR_LIST *users = NULL;
#if defined(WITH_DBM) || defined(WITH_NDBM)
        if (!use_dbm &&
            (checkdbm(filename, ".dir") == 0 ||
             checkdbm(filename, ".db") == 0)) {
                log(L_INFO|L_CONS, "DBM files found but no -b flag " "given - NOT using DBM");
        }
#endif

        if (!use_dbm) {
		rcode = pairlist_read(filename, &users, 1);
		if (rcode < 0) {
			return -1;
		}
	}

        /*
         *	Walk through the 'users' file list, if we're debugging,
	 *	or if we're in compat_mode.
         */
        if ((debug_flag) ||
	    (strcmp(config.compat_mode, "cistron") == 0)) {
                PAIR_LIST *entry;
                VALUE_PAIR *vp;
		int compat_mode = FALSE;

		if (strcmp(config.compat_mode, "cistron") == 0) {
			compat_mode = TRUE;
		}
        
                entry = users;
                while (entry) {
			if (compat_mode) {
				DEBUG("[%s]:%d Cistron compatibility checks for entry %s ...",
				      filename, entry->lineno,
				      entry->name);
			}

                        /*
                         *	Look for improper use of '=' in the
                         *	check items.  They should be using
                         *	'==' for on-the-wire RADIUS attributes,
                         *	and probably ':=' for server
                         *	configuration items.
                         */
                        for (vp = entry->check; vp != NULL; vp = vp->next) {
                                /*
                                 *	Ignore attributes which are set
                                 *	properly.
                                 */
                                if (vp->operator != T_OP_EQ) {
                                        continue;
                                }

                                /*
                                 *	If it's a vendor attribute,
                                 *	or it's a wire protocol, 
                                 *	ensure it has '=='.
                                 */
                                if (((vp->attribute & ~0xffff) != 0) ||
                                    (vp->attribute < 0x100)) {
					if (!compat_mode) {
						DEBUG("[%s]:%d WARNING! Changing '%s =' to '%s =='\n\tfor comparing RADIUS attribute in check item list for user %s",
						      filename, entry->lineno,
						      vp->name, vp->name,
						      entry->name);
					} else {
						DEBUG("\tChanging '%s =' to '%s =='",
						      vp->name, vp->name);
					}
					vp->operator = T_OP_CMP_EQ;
					continue;
                                }
				
				/*
				 *	Cistron Compatibility mode.
				 *
				 *	Re-write selected attributes
				 *	to be '+=', instead of '='.
				 *
				 *	All others get set to '=='
				 */
				if (compat_mode) {
					switch (vp->attribute) {
					default:
						DEBUG("\tChanging '%s =' to '%s =='",
						      vp->name, vp->name);
						vp->operator = T_OP_CMP_EQ;
						break;
						
					case PW_SIMULTANEOUS_USE:
					case PW_AUTHTYPE:
						DEBUG("\tChanging '%s =' to '%s +='",
						      vp->name, vp->name);
						vp->operator = T_OP_ADD;
						break;
					}
				}
				
                        } /* end of loop over check items */
                
                
                        /*
                         *	Look for server configuration items
                         *	in the reply list.
                         *
                         *	It's a common enough mistake, that it's
                         *	worth doing.
                         */
                        for (vp = entry->reply; vp != NULL; vp = vp->next) {
                                /*
                                 *	If it's NOT a vendor attribute,
                                 *	and it's NOT a wire protocol
                                 *	and we ignore Fall-Through,
                                 *	then bitch about it, giving a
                                 *	good warning message.
                                 */
                                if (!(vp->attribute & ~0xffff) &&
                                    (vp->attribute > 0xff) &&
                                    (vp->attribute > 1000)) {
                                        log_debug("[%s]:%d WARNING! Check item \"%s\"\n"
                                                  "\tfound in reply item list for user \"%s\".\n"
                                                  "\tThis attribute MUST go on the first line"
                                                  " with the other check items", 
                                                  filename, entry->lineno, vp->name,
                                                  entry->name);
                                }
                        }
                
                        entry = entry->next;
                }
        
        }

	*pair_list = users;
        return 0;
}

/*
 *	(Re-)read the "users" file into memory.
 */
static int file_instantiate(CONF_SECTION *conf, void **instance)
{
        struct file_instance *inst;
	int rcode;

        inst = malloc(sizeof *inst);
        if (!inst) {
                log(L_ERR|L_CONS, "Out of memory\n");
                return -1;
        }

        if (cf_section_parse(conf, module_config) < 0) {
                free(inst);
                return -1;
        }

        inst->detailperm = config.detailperm;
        inst->usersfile = config.usersfile;
        inst->acctusersfile = config.acctusersfile;
        config.usersfile = NULL;
        config.acctusersfile = NULL;

	rcode = getusersfile(inst->usersfile, &inst->users);
        if (rcode != 0) {
                log(L_ERR|L_CONS, "Errors reading %s", inst->usersfile);
                free(inst->usersfile);
                free(inst->acctusersfile);
                free(inst);
                return -1;
        }

	rcode = getusersfile(inst->acctusersfile, &inst->acctusers);
        if (rcode != 0) {
                log(L_ERR|L_CONS, "Errors reading %s", inst->acctusersfile);
                pairlist_free(&inst->users);
                free(inst->usersfile);
                free(inst->acctusersfile);
                free(inst);
                return -1;
        }

        *instance = inst;
        return 0;
}

/*
 *	Find the named user in the database.  Create the
 *	set of attribute-value pairs to check and reply with
 *	for this user from the database. The main code only
 *	needs to check the password, the rest is done here.
 */
static int file_authorize(void *instance, REQUEST *request,
		VALUE_PAIR **check_pairs, VALUE_PAIR **reply_pairs)
{
	int		nas_port = 0;
	VALUE_PAIR	*namepair;
	VALUE_PAIR	*request_pairs;
	VALUE_PAIR	*check_tmp;
	VALUE_PAIR	*reply_tmp;
	VALUE_PAIR	*tmp, *tmp2;
	PAIR_LIST	*pl;
	int		found = 0;
#if defined(WITH_DBM) || defined(WITH_NDBM)
	int		i, r;
	char		buffer[256];
#endif
	const char	*name;
	struct file_instance *inst = instance;

	request_pairs = request->packet->vps;

 	/*
	 *	Grab the canonical user name.
	 */
	namepair = request->username;
	name = namepair ? (char *) namepair->strvalue : "NONE";

	/*
	 *	Find the NAS port ID.
	 */
	if ((tmp = pairfind(request_pairs, PW_NAS_PORT_ID)) != NULL)
		nas_port = tmp->lvalue;

	/*
	 *	Find the entry for the user.
	 */
#if defined(WITH_DBM) || defined(WITH_NDBM)
	/*
	 *	FIXME: move to rlm_dbm.c
	 */
	if (use_dbm) {
		/*
		 *	FIXME: No Prefix / Suffix support for DBM.
		 */
#ifdef WITH_DBM
		if (dbminit(inst->usersfile) != 0)
#endif
#ifdef WITH_NDBM
		if ((dbmfile = dbm_open(inst->usersfile, O_RDONLY, 0)) == NULL)
#endif
		{
			log(L_ERR|L_CONS, "cannot open dbm file %s",
				buffer);
			return RLM_MODULE_FAIL;
		}

		r = dbm_find(dbmfile, name, request_pairs, check_pairs,
			     reply_pairs);
		if (r > 0) found = 1;
		if (r <= 0 || fallthrough(*reply_pairs)) {

			pairdelete(reply_pairs, PW_FALL_THROUGH);

			sprintf(buffer, "DEFAULT");
			i = 0;
			while ((r = dbm_find(dbmfile, buffer, request_pairs,
			       check_pairs, reply_pairs)) >= 0 || i < 2) {
				if (r > 0) {
					found = 1;
					if (!fallthrough(*reply_pairs))
						break;
					pairdelete(reply_pairs,PW_FALL_THROUGH);
				}
				sprintf(buffer, "DEFAULT%d", i++);
			}
		}
#ifdef WITH_DBM
		dbmclose();
#endif
#ifdef WITH_NDBM
		dbm_close(dbmfile);
#endif
	} else
	/*
	 *	Note the fallthrough through the #endif.
	 */
#endif

	for(pl = inst->users; pl; pl = pl->next) {

		/*
		 *	If the current entry is NOT a default,
		 *	AND the name does NOT match the current entry,
		 *	then skip to the next entry.
		 */
		if ((strcmp(pl->name, "DEFAULT") != 0) &&
		    (strcmp(name, pl->name) != 0))
			continue;

		/*
		 *	If the current request matches against the
		 *	check pairs, then add the reply pairs from the
		 *	entry to the current list of reply pairs.
		 */
		if (paircmp(request_pairs, pl->check, reply_pairs) == 0) {
			DEBUG2("  users: Matched %s at %d",
			       pl->name, pl->lineno);
			found = 1;
			check_tmp = paircopy(pl->check);
			reply_tmp = paircopy(pl->reply);
			pairmove(reply_pairs, &reply_tmp);
			pairmove(check_pairs, &check_tmp);
			pairfree(reply_tmp);
			pairfree(check_tmp); /* should be NULL */
			/*
			 *	Fallthrough?
			 */
			if (!fallthrough(pl->reply))
				break;
		}
	}

	/*
	 *	See if we succeeded.  If we didn't find the user,
	 *	then exit from the module.
	 */
	if (!found)
		return RLM_MODULE_OK;

	/*
	 *	Add the port number to the Framed-IP-Address if
	 *	vp->addport is set, or if the Add-Port-To-IP-Address
	 *	pair is present.
	 *
	 *	FIXME: this should not happen here, but
	 *	after module_authorize in the main code!
	 */
	if ((tmp = pairfind(*reply_pairs, PW_FRAMED_IP_ADDRESS)) != NULL) {
		tmp2 = pairfind(*reply_pairs, PW_ADD_PORT_TO_IP_ADDRESS);
		if (tmp->addport || (tmp2 && tmp2->lvalue)) {
			tmp->lvalue = htonl(ntohl(tmp->lvalue) + nas_port);
			tmp->addport = 0;
		}
		pairdelete(reply_pairs, PW_ADD_PORT_TO_IP_ADDRESS);
	}

	/*
	 *	Remove server internal parameters.
	 */
	pairdelete(reply_pairs, PW_FALL_THROUGH);

	return RLM_MODULE_OK;
}

/*
 *	Authentication - unused.
 */
static int file_authenticate(void *instance, REQUEST *request)
{
	instance = instance;
	request = request;
	return RLM_MODULE_OK;
}

/*
 * Write the dynamic log files
 */
static void file_write_dynamic_log(REQUEST * request)
{
	char fn[1024];
	char buffer[4096];
	int x,y;
	VALUE_PAIR * pair;
	FILE * f;

	pair = pairfind(request->packet->vps,PW_ACCT_STATUS_TYPE);
	for (x = 0; x < logcnt; x++) {
		if (((pair->lvalue == PW_STATUS_START) && (logcfg[x].flags & DL_FLAG_START)) ||
		    ((pair->lvalue == PW_STATUS_STOP) && (logcfg[x].flags & DL_FLAG_STOP)) ||
		    ((pair->lvalue == PW_STATUS_ACCOUNTING_ON) && (logcfg[x].flags & DL_FLAG_ACCT_ON)) ||
		    ((pair->lvalue == PW_STATUS_ACCOUNTING_OFF) && (logcfg[x].flags & DL_FLAG_ACCT_OFF)) ||
		    ((pair->lvalue == PW_STATUS_ALIVE) && (logcfg[x].flags & DL_FLAG_ALIVE))) {
			y = radius_xlat2(fn,sizeof(fn),logcfg[x].dir,request,request->packet->vps);
			(void) mkdir(fn, 0755);
			strcat(fn,"/");
			y++;
			/* FIXME must get the reply packet */
			radius_xlat2(&fn[y],sizeof(fn)-y,logcfg[x].fname,request,request->packet->vps);
			if (strcasecmp(logcfg[x].mode,"d") == 0) {
				remove(fn);
			} else {
				if (fn[y] == '|') {
					f = popen(&fn[y+1],logcfg[x].mode);
				} else {
					/* FIXME: permissions? */
					f = fopen(fn,logcfg[x].mode);
				}
				if (f) {
					/* FIXME must get the reply packet */
					radius_xlat2(buffer,sizeof(buffer),logcfg[x].fmt,request,request->packet->vps);
					fprintf(f,"%s\n",buffer);
					if (fn[y] == '|') {
						pclose(f);
					} else {
						fclose(f);
					}
				} else {
					if (fn[y] == '|') {
						log_debug("Error opening pipe %s",fn[y+1]);
					} else {
						log_debug("Error opening log %s",fn);
					}
				}
			}
		}


	}
}

/*
 *	Pre-Accounting - read the acct_users file for check_items and
 *	config_items. Reply items are Not Recommended(TM) in acct_users,
 *	except for Fallthrough, which should work
 *
 *	This function is mostly a copy of file_authorize
 */
static int file_preacct(void *instance, REQUEST *request)
{
	VALUE_PAIR	*namepair;
	const char	*name;
	VALUE_PAIR	*request_pairs;
	VALUE_PAIR	**config_pairs;
	VALUE_PAIR	*reply_pairs = NULL;
	VALUE_PAIR	*check_tmp;
	VALUE_PAIR	*reply_tmp;
	PAIR_LIST	*pl;
	int		found = 0;
#if defined(WITH_DBM) || defined(WITH_NDBM)
	int		i, r;
	char		buffer[256];
#endif
	struct file_instance *inst = instance;

	namepair = request->username;
	name = namepair ? (char *) namepair->strvalue : "NONE";
	request_pairs = request->packet->vps;
	config_pairs = &request->config_items;
	
	/*
	 *	Find the entry for the user.
	 */
#if defined(WITH_DBM) || defined(WITH_NDBM)
	/*
	 *	FIXME: move to rlm_dbm.c
	 */
	if (use_dbm) {
		/*
		 *	FIXME: No Prefix / Suffix support for DBM.
		 */
#ifdef WITH_DBM
		if (dbminit(inst->acctusersfile) != 0)
#endif
#ifdef WITH_NDBM
		if ((dbmfile = dbm_open(inst->acctusersfile, O_RDONLY, 0)) == NULL)
#endif
		{
			log(L_ERR|L_CONS, "cannot open dbm file %s",
				buffer);
			return RLM_MODULE_FAIL;
		}

		r = dbm_find(dbmfile, name, request_pairs, config_pairs,
			     &reply_pairs);
		if (r > 0) found = 1;
		if (r <= 0 || fallthrough(*reply_pairs)) {

		  pairdelete(reply_pairs, PW_FALL_THROUGH);

			sprintf(buffer, "DEFAULT");
			i = 0;
			while ((r = dbm_find(dbmfile, buffer, request_pairs,
			       config_pairs, &reply_pairs)) >= 0 || i < 2) {
				if (r > 0) {
					found = 1;
					if (!fallthrough(*reply_pairs))
						break;
					pairdelete(reply_pairs,PW_FALL_THROUGH);
				}
				sprintf(buffer, "DEFAULT%d", i++);
			}
		}
#ifdef WITH_DBM
		dbmclose();
#endif
#ifdef WITH_NDBM
		dbm_close(dbmfile);
#endif
	} else
	/*
	 *	Note the fallthrough through the #endif.
	 */
#endif

	for(pl = inst->acctusers; pl; pl = pl->next) {

		if (strcmp(name, pl->name) && strcmp(pl->name, "DEFAULT"))
			continue;

		if (paircmp(request_pairs, pl->check, &reply_pairs) == 0) {
			DEBUG2("  acct_users: Matched %s at %d",
			       pl->name, pl->lineno);
			found = 1;
			check_tmp = paircopy(pl->check);
			reply_tmp = paircopy(pl->reply);
			pairmove(&reply_pairs, &reply_tmp);
			pairmove(config_pairs, &check_tmp);
			pairfree(reply_tmp);
			pairfree(check_tmp); /* should be NULL */
			/*
			 *	Fallthrough?
			 */
			if (!fallthrough(pl->reply))
				break;
		}
	}

	/*
	 *	See if we succeeded.
	 */
	if (!found)
		return RLM_MODULE_OK; /* on to the next module */

	/*
	 *	FIXME: log a warning if there are any reply items other than
	 *	Fallthrough
	 */
	pairfree(reply_pairs); /* Don't need these */

	return RLM_MODULE_OK;
}

/*
 *	Accounting - write the detail files.
 */
static int file_accounting(void *instance, REQUEST *request)
{
	int		outfd;
	FILE		*outfp;
	char		nasname[128];
	char		buffer[512];
	VALUE_PAIR	*pair;
	uint32_t	nas;
	NAS		*cl;
	long		curtime;
	int		ret = RLM_MODULE_OK;
	struct stat	st;

	struct file_instance *inst = instance;

	/*
	 *	See if we have an accounting directory. If not,
	 *	return.
	 */
	if (stat(radacct_dir, &st) < 0) {
		DEBUG("No accounting directory %s", radacct_dir);
		return RLM_MODULE_OK;
	}
	curtime = time(0);

	/*
	 *	Find out the name of this terminal server. We try
	 *	to find the PW_NAS_IP_ADDRESS in the naslist file.
	 *	If that fails, we look for the originating address.
	 *	Only if that fails we resort to a name lookup.
	 */
	cl = NULL;
	nas = request->packet->src_ipaddr;
	if ((pair = pairfind(request->packet->vps, PW_NAS_IP_ADDRESS)) != NULL)
		nas = pair->lvalue;
	if (request->proxy && request->proxy->src_ipaddr)
		nas = request->proxy->src_ipaddr;

	if ((cl = nas_find(nas)) != NULL) {
		if (cl->shortname[0])
			strcpy(nasname, cl->shortname);
		else
			strcpy(nasname, cl->longname);
	}

	if (cl == NULL) {
		ip_hostname(nasname, sizeof(nasname), nas);
	}

	/*
	 *	Create a directory for this nas.
	 */
	sprintf(buffer, "%s/%s", radacct_dir, nasname);
	(void) mkdir(buffer, 0755);

	/*
	 *	Write Detail file.
	 */
	sprintf(buffer, "%s/%s/%s", radacct_dir, nasname, "detail");
	if ((outfd = open(buffer, O_WRONLY|O_APPEND|O_CREAT,
			  inst->detailperm)) < 0) {
		log(L_ERR, "Acct: Couldn't open file %s", buffer);
		ret = RLM_MODULE_FAIL;
	} else if ((outfp = fdopen(outfd, "a")) == NULL) {
		log(L_ERR, "Acct: Couldn't open file %s: %s",
		    buffer, strerror(errno));
		ret = RLM_MODULE_FAIL;
		close(outfd);
	} else {

		/* Post a timestamp */
		fputs(ctime(&curtime), outfp);

		/* Write each attribute/value to the log file */
		pair = request->packet->vps;
		while (pair) {
			if (pair->attribute != PW_PASSWORD) {
				fputs("\t", outfp);
				fprint_attr_val(outfp, pair);
				fputs("\n", outfp);
			}
			pair = pair->next;
		}

		/*
		 *	Add non-protocol attibutes.
		 */
		fprintf(outfp, "\tTimestamp = %ld\n", curtime);
		if (request->packet->verified)
			fputs("\tRequest-Authenticator = Verified\n", outfp);
		else
			fputs("\tRequest-Authenticator = None\n", outfp);
		fputs("\n", outfp);
		fclose(outfp);
	}
	file_write_dynamic_log(request);
	return ret;
}


/*
 *	Clean up.
 */
static int file_detach(void *instance)
{
        struct file_instance *inst = instance;
        pairlist_free(&inst->users);
        pairlist_free(&inst->acctusers);
        free(inst->usersfile);
        free(inst->acctusersfile);
        free(inst);
	return 0;
}


/* globally exported name */
module_t rlm_files = {
	"files",
	0,				/* type: reserved */
	file_init,			/* initialization */
	file_instantiate,		/* instantiation */
	file_authorize, 		/* authorization */
	file_authenticate,		/* authentication */
	file_preacct,			/* preaccounting */
	file_accounting,		/* accounting */
	file_detach,			/* detach */
	NULL				/* destroy */
};

