/*****************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2006 Lubos Lunak <l.lunak@kde.org>

You can Freely distribute this program under the GNU General Public
License. See the file "COPYING" for the exact licensing terms.
******************************************************************/

#include "toplevel.h"

namespace KWinInternal
{

Toplevel::Toplevel( Workspace* ws )
    : vis( None )
    , id( None )
    , wspace( ws )
    , window_pix( None )
    , damage_handle( None )
    , is_shape( false )
    {
    }

Toplevel::~Toplevel()
    {
    assert( damage_handle == None );
    discardWindowPixmap();
    }

#ifndef NDEBUG
kdbgstream& operator<<( kdbgstream& stream, const Toplevel* cl )
    {
    if( cl == NULL )
        return stream << "\'NULL\'";
    cl->debug( stream );
    return stream;
    }

kdbgstream& operator<<( kdbgstream& stream, const ToplevelList& list )
    {
    stream << "LIST:(";
    bool first = true;
    for( ToplevelList::ConstIterator it = list.begin();
         it != list.end();
         ++it )
        {
        if( !first )
            stream << ":";
        first = false;
        stream << *it;
        }
    stream << ")";
    return stream;
    }

kdbgstream& operator<<( kdbgstream& stream, const ConstToplevelList& list )
    {
    stream << "LIST:(";
    bool first = true;
    for( ConstToplevelList::ConstIterator it = list.begin();
         it != list.end();
         ++it )
        {
        if( !first )
            stream << ":";
        first = false;
        stream << *it;
        }
    stream << ")";
    return stream;
    }
#endif

void Toplevel::detectShape( Window id )
    {
    is_shape = Extensions::hasShape( id );
    }

// used only by Deleted::copy()
void Toplevel::copyToDeleted( Toplevel* c )
    {
    geom = c->geom;
    vis = c->vis;
    bit_depth = c->bit_depth;
    id = c->id;
    wspace = c->wspace;
    window_pix = c->window_pix;
    c->window_pix = None;
    damage_handle = None;
    damage_region = c->damage_region;
    is_shape = c->is_shape;
    }

} // namespace

#include "toplevel.moc"
