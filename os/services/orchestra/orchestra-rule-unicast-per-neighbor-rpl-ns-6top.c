/*
 * Copyright (c) 2016, Inria.
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
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/**
 * \file
 *         Orchestra: a slotframe dedicated to unicast data transmission. Designed primarily
 *         for RPL non-storing mode but would work with any mode-of-operation. Does not require
 *         any knowledge of the children. Works only as received-base, and as follows:
 *           Nodes listen at a timeslot defined as hash(MAC) % ORCHESTRA_SB_UNICAST_PERIOD
 *           Nodes transmit at: for any neighbor, hash(nbr.MAC) % ORCHESTRA_SB_UNICAST_PERIOD
 *
 * \author Simon Duquennoy <simon.duquennoy@inria.fr>
 */

#include "contiki.h"
#include "orchestra.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "moudle/sixtop_simple_schdule/sf-simple.h"

#include <stdio.h>

#if PROJECT_CHILD_LIST_HACK
#include "moudle/self_maintain_childlist/childlist.h"
#endif

static uint16_t slotframe_handle = 0;
static uint16_t channel_offset = 0;
static struct tsch_slotframe *sf_unicast_sixtop;

/*---------------------------------------------------------------------------*/
static uint16_t
get_node_timeslot(const linkaddr_t *addr)
{
  if(addr != NULL && ORCHESTRA_SIXTOP_PERIOD > 0) {
    return ORCHESTRA_LINKADDR_HASH(addr) % ORCHESTRA_SIXTOP_PERIOD;
  } else {
    return 0xffff;
  }
}
/*---------------------------------------------------------------------------*/
static void
add_uc_link(const linkaddr_t *linkaddr)
{
  if(linkaddr != NULL) {
    uint16_t timeslot = get_node_timeslot(linkaddr);
    uint8_t link_options = LINK_OPTION_SHARED | LINK_OPTION_RX;
    if(timeslot == get_node_timeslot(&linkaddr_node_addr)) {
      /* This is also our timeslot, add necessary flags */
      link_options |= LINK_OPTION_TX;
    }
    child_node *node;
    node = find_child(linkaddr);
    
    printf("Timeslot:%d USED:%d\n",timeslot,slot_is_used(timeslot));
    //print_child_list();
    
    if(node){
      if(SF_SIX_TOP_ENABLE_RELOCATE?(!slot_is_used(timeslot)):1){
      child_list_set_child_offsets(node,timeslot,channel_offset);
     /* Add/update link */
      tsch_schedule_add_link(sf_unicast_sixtop, link_options, LINK_TYPE_NORMAL, &tsch_broadcast_address,
          timeslot, channel_offset);
    }
    else
    {
      child_list_set_child_offsets(node,timeslot,channel_offset);
      if(sf_simple_realocate_links((linkaddr_t *)linkaddr,timeslot,channel_offset)<0){
         /* Add/update link */
        tsch_schedule_add_link(sf_unicast_sixtop, link_options, LINK_TYPE_NORMAL, &tsch_broadcast_address,
          timeslot, channel_offset);
      }
    }
    }
  }
}
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
static void
child_added(const linkaddr_t *linkaddr)
{
  add_uc_link(linkaddr);
}
/*---------------------------------------------------------------------------*/
static void
child_removed(const linkaddr_t *linkaddr)
{
}
/*---------------------------------------------------------------------------*/
static int
select_packet(uint16_t *slotframe, uint16_t *timeslot)
{
  /* Select data packets we have a unicast link to */
  const linkaddr_t *dest = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
  //printf("6top_sb: %d %d %d\n",packetbuf_attr(PACKETBUF_ATTR_NETWORK_ID),packetbuf_attr(PACKETBUF_ATTR_INSIDE_PROTO),linkaddr_cmp(&orchestra_parent_linkaddr, dest));
  if(packetbuf_attr(PACKETBUF_ATTR_FRAME_TYPE) == FRAME802154_DATAFRAME
     && !linkaddr_cmp(dest, &linkaddr_null) && linkaddr_cmp(&orchestra_parent_linkaddr, dest) 
     && packetbuf_attr(PACKETBUF_ATTR_INSIDE_PROTO) == UIP_PROTO_UDP ) {
     
    if(slotframe != NULL) {
      *slotframe = slotframe_handle;
    }
    if(timeslot != NULL) {
      child_node *node = find_child(&linkaddr_node_addr);
      if(node){
      *timeslot = node -> slot_offset;
      }
      else
      {
        return 0;
      }
    }
    //printf("send by 6top_sb! %d\n",(int)timeslot);
    return 1;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static void
new_time_source(const struct tsch_neighbor *old, const struct tsch_neighbor *new)
{
  if(new != old) {
    const linkaddr_t *old_addr = old != NULL ? &old->addr : NULL;
    const linkaddr_t *new_addr = new != NULL ? &new->addr : NULL;
    if(new_addr != NULL) {
      linkaddr_copy(&orchestra_parent_linkaddr, new_addr);
    } else {
      linkaddr_copy(&orchestra_parent_linkaddr, &linkaddr_null);
    }
    if(old_addr != NULL && new_addr != NULL){
    sf_simple_switching_parent_callback((linkaddr_t *)old_addr,(linkaddr_t *)new_addr,get_node_timeslot(&linkaddr_node_addr));
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
init(uint16_t sf_handle)
{
  uint16_t tx_timeslot;
  slotframe_handle = sf_handle;
  channel_offset = sf_handle;
  sf_set_slotframe_handle(sf_handle);
  
  sixtop_add_sf(&sf_simple_driver);
  child_list_ini();
  /* Slotframe for unicast transmissions */
  sf_unicast_sixtop = tsch_schedule_add_slotframe(slotframe_handle, ORCHESTRA_SIXTOP_PERIOD);
  tx_timeslot = get_node_timeslot(&linkaddr_node_addr);
  sf_set_default_slot(tx_timeslot);
  child_node *node;
    node = child_list_add_child(&linkaddr_node_addr);
    if(node){
      child_list_set_child_offsets(node,tx_timeslot,channel_offset);
    }
  
    tsch_schedule_add_link(sf_unicast_sixtop,
        LINK_OPTION_SHARED | LINK_OPTION_TX,
        LINK_TYPE_NORMAL, &tsch_broadcast_address,
        tx_timeslot, channel_offset);
  
}
/*---------------------------------------------------------------------------*/
struct orchestra_rule unicast_per_neighbor_rpl_ns_6top = {
  init,
  new_time_source,
  select_packet,
  child_added,
  child_removed,
};
