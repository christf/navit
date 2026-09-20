//
// Created by jan on 18.02.26.
//

#ifndef NAVIT_GATOMIC_H
#define NAVIT_GATOMIC_H

#include "gtypes.h"

gint g_atomic_int_exchange_and_add(volatile gint *atomic, gint val);
void g_atomic_int_add(volatile gint *atomic, gint val);
gboolean g_atomic_int_compare_and_exchange(volatile gint *atomic, gint oldval, gint newval);
gboolean g_atomic_pointer_compare_and_exchange(volatile gpointer *atomic, gpointer oldval, gpointer newval);
gint g_atomic_int_get(volatile gint *atomic);
void g_atomic_int_set(volatile gint *atomic, gint newval);
gpointer g_atomic_pointer_get(volatile gpointer *atomic);
void g_atomic_pointer_set(volatile gpointer *atomic, gpointer newval);

#endif //NAVIT_GATOMIC_H
