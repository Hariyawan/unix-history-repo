/*-
 * Copyright (c) 1998 Michael Smith <msmith@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <stand.h>
#include <sys/disklabel.h>
#include "libofw.h"

static int	ofw_parsedev(struct ofw_devdesc **, const char *, const char **);

/* 
 * Point (dev) at an allocated device specifier for the device matching the
 * path in (devspec). If it contains an explicit device specification,
 * use that.  If not, use the default device.
 */
int
ofw_getdev(void **vdev, const char *devspec, const char **path)
{
    struct ofw_devdesc **dev = (struct ofw_devdesc **)vdev;
    int				rv;
    
    /*
     * If it looks like this is just a path and no
     * device, go with the current device.
     */
    if ((devspec == NULL) || 
	(devspec[0] == '/') || 
	(strchr(devspec, ':') == NULL)) {

	if (((rv = ofw_parsedev(dev, getenv("currdev"), NULL)) == 0) &&
	    (path != NULL))
		*path = devspec;
	return(rv);
    }
    
    /*
     * Try to parse the device name off the beginning of the devspec
     */
    return(ofw_parsedev(dev, devspec, path));
}

/*
 * Point (dev) at an allocated device specifier matching the string version
 * at the beginning of (devspec).  Return a pointer to the remaining
 * text in (path).
 *
 * In all cases, the beginning of (devspec) is compared to the names
 * of known devices in the device switch, and then any following text
 * is parsed according to the rules applied to the device type.
 *
 * For disk-type devices, the syntax is:
 *
 * disk<unit>[s<slice>][<partition>]:
 * 
 */
static int
ofw_parsedev(struct ofw_devdesc **dev, const char *devspec, const char **path)
{
    struct ofw_devdesc *idev;
    struct devsw	*dv;
    int			i, unit, slice, partition, err;
    char		*cp;
    const char		*np;

    /* minimum length check */
    if (strlen(devspec) < 2)
	return(EINVAL);

    /* look for a device that matches */
    for (i = 0, dv = NULL; devsw[i] != NULL; i++) {
	if (!strncmp(devspec, devsw[i]->dv_name, strlen(devsw[i]->dv_name))) {
	    dv = devsw[i];
	    break;
	}
    }

    if (dv == NULL)
	return(ENOENT);
    idev = malloc(sizeof(struct ofw_devdesc));
    err = 0;
    np = (devspec + strlen(dv->dv_name));
        
    switch(dv->dv_type) {
    case DEVT_NONE:			/* XXX what to do here?  Do we care? */
	break;

    case DEVT_DISK:
	unit = -1;
	slice = -1;
	partition = -1;
	if (*np && (*np != ':')) {
	    unit = strtol(np, &cp, 10);	/* next comes the unit number */
	    if (cp == np) {
		err = EUNIT;
		goto fail;
	    }
	    if (*cp == 's') {		/* got a slice number */
		np = cp + 1;
		slice = strtol(np, &cp, 10);
		if (cp == np) {
		    err = ESLICE;
		    goto fail;
		}
	    }
	    if (*cp && (*cp != ':')) {
		partition = *cp - 'a';		/* get a partition number */
		if ((partition < 0) || (partition >= MAXPARTITIONS)) {
		    err = EPART;
		    goto fail;
		}
		cp++;
	    }
	}
	if (*cp && (*cp != ':')) {
	    err = EINVAL;
	    goto fail;
	}

	idev->d_kind.ofwdisk.unit = unit;
	idev->d_kind.ofwdisk.slice = slice;
	idev->d_kind.ofwdisk.partition = partition;
	if (path != NULL)
	    *path = (*cp == 0) ? cp : cp + 1;
	break;
	
    case DEVT_NET:
	unit = 0;
	
	if (*np && (*np != ':')) {
	    unit = strtol(np, &cp, 0);	/* get unit number if present */
	    if (cp == np) {
		err = EUNIT;
		goto fail;
	    }
	}
	if (*cp && (*cp != ':')) {
	    err = EINVAL;
	    goto fail;
	}
	
	idev->d_kind.netif.unit = unit;
	if (path != NULL)
	    *path = (*cp == 0) ? cp : cp + 1;
	break;

    default:
	err = EINVAL;
	goto fail;
    }
    idev->d_dev = dv;
    idev->d_type = dv->dv_type;
    if (dev == NULL) {
	free(idev);
    } else {
	*dev = idev;
    }
    return(0);

 fail:
    free(idev);
    return(err);
}

/* hack to correctly parse bootpath for currdev. */
int
ofw_parseofwdev(struct ofw_devdesc *dev, const char *devspec)
{
	char *cp, *ep;
	int i;
	struct devsw *dv;

	if ((cp = strrchr(devspec, '@')) == 0)
	    return EINVAL;
	cp++;
	ep = cp;
	dev->d_kind.ofwdisk.unit = strtol(cp, &cp, 10);
	if (cp == ep)
	    return EUNIT;
	if (*cp != ',')
	    return ESLICE;
	ep = ++cp;
	dev->d_kind.ofwdisk.slice = strtol(cp, &cp, 10) + 1;
	if (cp == ep)
	    return ESLICE;
	if (*cp != ':')
	    return EPART;
	dev->d_kind.ofwdisk.partition = *++cp - 'a';
}

char *
ofw_fmtdev(void *vdev)
{
	struct ofw_devdesc	*dev = (struct ofw_devdesc *)vdev;
	static char		buf[128];
	char			*cp;

	switch(dev->d_type) {
	case DEVT_NONE:
		strcpy(buf, "(no device)");
		break;

	case DEVT_DISK:
		sprintf(buf, "%s%ds%d%c:", dev->d_dev->dv_name,
		    dev->d_kind.ofwdisk.unit, dev->d_kind.ofwdisk.slice,
		    dev->d_kind.ofwdisk.partition + 'a');
		break;

	case DEVT_NET:
		sprintf(buf, "%s%d:", dev->d_dev->dv_name,
		    dev->d_kind.netif.unit);
		break;
	}

	return buf;
}

int
ofw_setcurrdev(struct env_var *ev, int flags, void *value)
{
	struct ofw_devdesc	*ncurr;
	int			rv;

	if ((rv = ofw_parsedev(&ncurr, value, NULL)) != 0)
		return rv;

	free(ncurr);
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);
	return 0;
}
