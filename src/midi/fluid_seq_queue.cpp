/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2019  Tom Moebert and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#include "fluid_seq_queue.h"

#include <deque>
#include <algorithm>

/*
 * This is an implementation of an event queue, sorted according to their timestamp.
 *
 * Very similar to std::priority_queue.
 *
 * However, we cannot use std::priority_queue, because fluid_sequencer_remove_events() requires
 * access to the underlying container, which however std::priority_queue doesn't provide (without hacks).
 * On the other hand, implementing a priority_queue while managing our own heap structure is very
 * simple to do using std::make_heap() and friends.
 */

// we are using a deque rather than vector, because:
// "Expansion of a deque is cheaper than the expansion of a std::vector because it does not involve
// copying of the existing elements to a new memory location."
typedef std::deque<fluid_event_t> seq_queue_t;

static bool event_compare(const fluid_event_t& left, const fluid_event_t& right)
{
    bool leftIsBeforeRight;

    unsigned int ltime = left.time, rtime = right.time;
    if(ltime < rtime)
    {
        leftIsBeforeRight = true;
    }
    else if (ltime == rtime)
    {
        fluid_seq_event_type ltype = static_cast<fluid_seq_event_type>(left.type);
        fluid_seq_event_type rtype = static_cast<fluid_seq_event_type>(right.type);

        // Both events have the same tick value. The order is undefined.
        //
        // We do the following:
        //  * System reset events are always first,
        //  * unregistering events are second.
        // This gives clients the chance to reset themselves before unregistering at the same tick.
        //
        // NoteOn events are always last. This makes sure that all other "state-change" events have been processed and NoteOff events
        // with the same key as the NoteOn have been processed (to avoid zero-length notes).
        // For any other event type, the order is undefined.
        // See:
        // https://lists.nongnu.org/archive/html/fluid-dev/2019-12/msg00001.html
        // https://lists.nongnu.org/archive/html/fluid-dev/2017-05/msg00004.html
        //
        // The expression below ensures the order described above.
        // This boolean expression is written in disjunctive normal form and can be obtained by using the Karnaugh map below,
        // which contains
        //  * possible values of rtype in columns,
        //  * possible values of ltype in rows,
        //  * the boolean values to indicate whether leftIsBeforeRight, and
        //  * X meaning any other event type.
        //
        // | ltype \ rtype | SYSR | UNREG | NOTEON | X |
        // |      SYSR     |  1   |   1   |   1    | 1 |
        // |     UNREG     |  0   |   1   |   1    | 1 |
        // |     NOTEON    |  0   |   0   |   1    | 0 |
        // |       X       |  0   |   0   |   1    | 1 |
        //
        // The values in the diagonal (i.e. comparision with itself) must be true to make them return false after leaving this
        // function in order to satisfy the irreflexive requirement, i.e. assert(!(a < a))
        leftIsBeforeRight =
           // first row in table
           (ltype == FLUID_SEQ_SYSTEMRESET)
           // the rtype NOTEON column
        || (rtype == FLUID_SEQ_NOTEON || rtype == FLUID_SEQ_NOTE)
           // the second row in table
        || (rtype != FLUID_SEQ_SYSTEMRESET && ltype == FLUID_SEQ_UNREGISTERING)
           // the bottom right value, any other type compared to any other type
        || (ltype != FLUID_SEQ_SYSTEMRESET && ltype != FLUID_SEQ_UNREGISTERING && ltype != FLUID_SEQ_NOTEON && ltype != FLUID_SEQ_NOTE &&
            rtype != FLUID_SEQ_SYSTEMRESET && rtype != FLUID_SEQ_UNREGISTERING && rtype != FLUID_SEQ_NOTEON && rtype != FLUID_SEQ_NOTE);
    }
    else
    {
        leftIsBeforeRight = false;
    }

    // we must negate the return value, because we are building a max-heap, i.e.
    // the smallest element is at ::front()
    return !leftIsBeforeRight;
}


void* new_fluid_seq_queue(int nb_events)
{
    try
    {
        // As workaround for a missing deque::reserve(), allocate a deque with a size of nb_events
        seq_queue_t* queue = new seq_queue_t(nb_events);
        // and clear the queue again
        queue->clear();
        // C++11 introduced a deque::shrink_to_fit(), so hopefully not calling shrink_to_fit() will
        // leave the previously used memory allocated, avoiding reallocation for subsequent insertions.

        return queue;
    }
    catch(...)
    {
        return 0;
    }
}

void delete_fluid_seq_queue(void *que)
{
    delete static_cast<seq_queue_t*>(que);
}

int fluid_seq_queue_push(void *que, const fluid_event_t *evt)
{
    try
    {
        seq_queue_t& queue = *static_cast<seq_queue_t*>(que);

        queue.push_back(*evt);
        std::push_heap(queue.begin(), queue.end(), event_compare);

        return FLUID_OK;
    }
    catch(...)
    {
        return FLUID_FAILED;
    }
}

void fluid_seq_queue_remove(void *que, fluid_seq_id_t src, fluid_seq_id_t dest, int type)
{
    seq_queue_t& queue = *static_cast<seq_queue_t*>(que);

    if(src == -1 && dest == -1 && type == -1)
    {
        // shortcut for deleting everything
        queue.clear();
    }
    else
    {
        for (seq_queue_t::iterator it = queue.begin(); it != queue.end();)
        {
            if((src == -1 || it->src == src) &&
            (dest == -1 || it->dest == dest) &&
            (type == -1 || it->type == type))
            {
                it = queue.erase(it);
            }
            else
            {
                ++it;
            }
        }

        std::make_heap(queue.begin(), queue.end(), event_compare);
    }
}

static void fluid_seq_queue_pop(seq_queue_t &queue)
{
    std::pop_heap(queue.begin(), queue.end(), event_compare);
    queue.pop_back();
}

void fluid_seq_queue_process(void *que, fluid_sequencer_t *seq)
{
    seq_queue_t& queue = *static_cast<seq_queue_t*>(que);

    unsigned int ticks = fluid_sequencer_get_tick(seq);
    while(!queue.empty())
    {
        // Get the top most event.
        const fluid_event_t& top = queue.front();
        if(top.time <= fluid_sequencer_get_tick(seq))
        {
            // First, copy it to a local buffer.
            // This is required because the content of the queue should be read-only to the client,
            // however, most client function receive a non-const fluid_event_t pointer
            fluid_event_t local_evt = top;

            // Then, pop the queue, so that client-callbacks may add new events without
            // messing up the heap structure while we are still processing
            fluid_seq_queue_pop(queue);
            fluid_sequencer_send_now(seq, &local_evt);
        }
        else
        {
            break;
        }
    }
}
