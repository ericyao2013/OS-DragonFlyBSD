#ifndef GNode_h_39503bf2
#define	GNode_h_39503bf2
/*
 * Copyright (c) 2004, 2005 by Max Okumoto
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Adam de Boor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/usr.bin/make/GNode.h,v 1.1 2005/01/06 11:41:47 okumoto Exp $
 */

#include "sprite.h"
#include "lst.h"

struct _Suff;

/*-
 * The structure for an individual graph node. Each node has several
 * pieces of data associated with it.
 *	1) the name of the target it describes
 *	2) the location of the target file in the filesystem.
 *	3) the type of operator used to define its sources (qv. parse.c)
 *	4) whether it is involved in this invocation of make
 *	5) whether the target has been remade
 *	6) whether any of its children has been remade
 *	7) the number of its children that are, as yet, unmade
 *	8) its modification time
 *	9) the modification time of its youngest child (qv. make.c)
 *	10) a list of nodes for which this is a source
 *	11) a list of nodes on which this depends
 *	12) a list of nodes that depend on this, as gleaned from the
 *	    transformation rules.
 *	13) a list of nodes of the same name created by the :: operator
 *	14) a list of nodes that must be made (if they're made) before
 *	    this node can be, but that do no enter into the datedness of
 *	    this node.
 *	15) a list of nodes that must be made (if they're made) after
 *	    this node is, but that do not depend on this node, in the
 *	    normal sense.
 *	16) a Lst of ``local'' variables that are specific to this target
 *	   and this target only (qv. var.c [$@ $< $?, etc.])
 *	17) a Lst of strings that are commands to be given to a shell
 *	   to create this target.
 */
typedef struct GNode {
	char	*name;	/* The target's name */
	char	*path;	/* The full pathname of the file */
	int	type;	/* Its type (see the OP flags, below) */
	int	order;	/* Its wait weight */

	Boolean	make;	/* TRUE if this target needs to be remade */

	/* Set to reflect the state of processing on this node */
	enum {
		UNMADE,		/* Not examined yet */

		BEINGMADE,	/* Target is already being made.
				 * Indicates a cycle in the graph.
				 * (compat mode only) */

		MADE,		/* Was out-of-date and has been made */

		UPTODATE,	/* Was already up-to-date */

		ERROR,		/* An error occured while it was being
				 * made (used only in compat mode) */

		ABORTED,	/* The target was aborted due to an
				 * error making an inferior (compat). */

		CYCLE,		/* Marked as potentially being part of
				 * a graph cycle.  If we come back to
				 * a node marked this way, it is printed
				 * and 'made' is changed to ENDCYCLE. */

		ENDCYCLE	/* The cycle has been completely printed.
				 * Go back and unmark all its members. */
	} made;

	Boolean	childMade;	/* TRUE if one of this target's children
				 * was made */
	int	unmade;		/* The number of unmade children */
	int	mtime;		/* Its modification time */
	int	cmtime;		/* Modification time of its youngest child */
	Lst	iParents;	/* Links to parents for which this is an
				 * implied source, if any */
	Lst	cohorts;	/* Other nodes for the :: operator */
	Lst	parents;	/* Nodes that depend on this one */
	Lst	children;	/* Nodes on which this one depends */
	Lst	successors;	/* Nodes that must be made after this one */
	Lst	preds;		/* Nodes that must be made before this one */

	Lst	context;	/* The local variables */
	Lst	commands;	/* Creation commands */

	/* current command executing in compat mode */
	LstNode	*compat_command;

	struct _Suff	*suffix;	/* Suffix for the node (determined
					 * by Suff_FindDeps and opaque to
					 * everyone but the Suff module) */
} GNode;

#endif /* GNode_h_39503bf2 */
