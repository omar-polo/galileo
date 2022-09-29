#include_next "unistd.h"

#include "../config.h"

#if !HAVE_PLEGDE
int	pledge(const char *, const char *);
#endif

#if !HAVE_UNVEIL
int	unveil(const char *, const char *);
#endif

#if !HAVE_GETDTABLECOUNT
int	getdtablecount(void);
#endif

#if !HAVE_GETDTABLESIZE
int	getdtablesize(void);
#endif
