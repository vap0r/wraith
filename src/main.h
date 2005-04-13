#ifndef _MAIN_H
#define _MAIN_H

#include <sys/types.h>

enum {
  UPDATE_AUTO = 1,
  UPDATE_EXIT
};

extern int		role, default_flags, default_uflags, do_confedit,
			updating;
extern bool		use_stderr, backgrd, used_B, term_z, loading, have_take;
extern char		tempdir[], *binname, owner[], version[], ver[], quit_msg[];
extern time_t		online_since, now;
extern uid_t		myuid;
extern pid_t            mypid;
extern const time_t	buildts;
extern const char	*egg_version;

void fatal(const char *, int);

#endif /* !_MAIN_H */
