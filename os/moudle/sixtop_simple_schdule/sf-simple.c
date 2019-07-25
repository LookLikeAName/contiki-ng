/*
 * Copyright (c) 2016, Yasuyuki Tanaka
 * Copyright (c) 2016, Centre for Development of Advanced Computing (C-DAC).
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
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * \file
 *         A 6P Simple Schedule Function
 * \author
 *         Shalu R <shalur@cdac.in>
 *         Lijo Thomas <lijo@cdac.in>
 *         Yasuyuki Tanaka <yasuyuki.tanaka@inf.ethz.ch>
 */

#include "contiki-lib.h"

#include "lib/assert.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "net/mac/tsch/sixtop/sixp.h"
#include "net/mac/tsch/sixtop/sixp-nbr.h"
#include "net/mac/tsch/sixtop/sixp-pkt.h"
#include "net/mac/tsch/sixtop/sixp-trans.h"

#include "moudle/self_maintain_childlist/childlist.h"
#include "sf-simple.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "6top"
#define LOG_LEVEL LOG_LEVEL_6TOP

#define DEBUG DEBUG_PRINT
#include "net/net-debug.h"
#include "stdio.h"

PROCESS(sf_wait_parent_switch_done_process, "sf wait parent switch done process");
process_event_t sf_parent_switch_done;
PROCESS(sf_wait_for_retry_process, "Wait for retry process");
PROCESS(sf_link_maintain_and_realocate_retry, "Start maintain links");

typedef struct {
  uint16_t timeslot_offset;
  uint16_t channel_offset;
} sf_simple_cell_t;

static uint16_t slotframe_handle = 0;
static uint16_t self_default_slot = 0;
static uint8_t res_storage[4 + SF_SIMPLE_MAX_LINKS * 4];
static uint8_t req_storage[4 + SF_SIMPLE_MAX_LINKS * 4];

static void read_cell(const uint8_t *buf, sf_simple_cell_t *cell);
static void print_cell_list(const uint8_t *cell_list, uint16_t cell_list_len);
static void add_links_to_schedule(const linkaddr_t *peer_addr,
                                  uint8_t link_option,
                                  const uint8_t *cell_list,
                                  uint16_t cell_list_len);
static void remove_links_to_schedule(const uint8_t *cell_list,
                                     uint16_t cell_list_len);
static void add_response_sent_callback(void *arg, uint16_t arg_len,
                                       const linkaddr_t *dest_addr,
                                       sixp_output_status_t status);
static void delete_response_sent_callback(void *arg, uint16_t arg_len,
                                          const linkaddr_t *dest_addr,
                                          sixp_output_status_t status);
static void realocate_response_sent_callback(void *arg, uint16_t arg_len,
                                       const linkaddr_t *dest_addr,
                                       sixp_output_status_t status);                                          
static void add_req_input(const uint8_t *body, uint16_t body_len,
                          const linkaddr_t *peer_addr);
static void delete_req_input(const uint8_t *body, uint16_t body_len,
                             const linkaddr_t *peer_addr);
static void realocate_req_input(const uint8_t *body, uint16_t body_len,
                          const linkaddr_t *peer_addr);
static void input(sixp_pkt_type_t type, sixp_pkt_code_t code,
                  const uint8_t *body, uint16_t body_len,
                  const linkaddr_t *src_addr);
static void request_input(sixp_pkt_cmd_t cmd,
                          const uint8_t *body, uint16_t body_len,
                          const linkaddr_t *peer_addr);
static void response_input(sixp_pkt_rc_t rc,
                           const uint8_t *body, uint16_t body_len,
                           const linkaddr_t *peer_addr);


int sf_set_slotframe_handle(uint16_t handle){
  slotframe_handle = handle;
  return (int)slotframe_handle;
}
int sf_set_default_slot(uint16_t slot){
  self_default_slot = slot;
  return (int)self_default_slot;
}


/*
 * scheduling policy:
 * add: if and only if all the requested cells are available, accept the request
 * delete: if and only if all the requested cells are in use, accept the request
 */

static void
read_cell(const uint8_t *buf, sf_simple_cell_t *cell)
{
  cell->timeslot_offset = buf[0] + (buf[1] << 8);
  cell->channel_offset = buf[2] + (buf[3] << 8);
}

static void
print_cell_list(const uint8_t *cell_list, uint16_t cell_list_len)
{
  uint16_t i;
  sf_simple_cell_t cell;

  for(i = 0; i < (cell_list_len / sizeof(cell)); i++) {
    read_cell(cell_list+(i*sizeof(cell)), &cell);
    PRINTF("[%u ", cell.timeslot_offset);
    PRINTF(" %u] ", cell.channel_offset);
  }
  PRINTF("\n");
}

static void
add_links_to_schedule(const linkaddr_t *peer_addr, uint8_t link_option,
                      const uint8_t *cell_list, uint16_t cell_list_len)
{
  /* add only the first valid cell */

  sf_simple_cell_t cell;
  struct tsch_slotframe *slotframe;
  int i;

  assert(cell_list != NULL);

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);

  if(slotframe == NULL) {
    return;
  }

  for(i = 0; i < (cell_list_len / sizeof(cell)); i++) {
    read_cell(cell_list+(i*sizeof(cell)), &cell);
    if(cell.timeslot_offset == 0xffff) {
      continue;
    }

    LOG_INFO("sf-simple: Schedule link %d as %s with node %u\n",
           cell.timeslot_offset,
           link_option == LINK_OPTION_RX ? "RX" : "TX",
           peer_addr->u8[7]);
    tsch_schedule_add_link(slotframe,
                           link_option, LINK_TYPE_NORMAL, &tsch_broadcast_address,
                           cell.timeslot_offset, cell.channel_offset);
    break;
  }
  //tsch_schedule_print();
}

static void
remove_links_to_schedule(const uint8_t *cell_list, uint16_t cell_list_len)
{
  /* remove all the cells */

  sf_simple_cell_t cell;
  struct tsch_slotframe *slotframe;
  int i;

  assert(cell_list != NULL);

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);

  if(slotframe == NULL) {
    return;
  }

  for(i = 0; i < (cell_list_len / sizeof(cell)); i++) {
    read_cell(cell_list+(i*sizeof(cell)), &cell);
    if(cell.timeslot_offset == 0xffff) {
      continue;
    }

    tsch_schedule_remove_link_by_timeslot(slotframe,
                                          cell.timeslot_offset);
  }
  //tsch_schedule_print();
}

static void
add_response_sent_callback(void *arg, uint16_t arg_len,
                           const linkaddr_t *dest_addr,
                           sixp_output_status_t status)
{
  uint8_t *body = (uint8_t *)arg;
  uint16_t body_len = arg_len;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  sixp_nbr_t *nbr;

  assert(body != NULL && dest_addr != NULL);

  if(status == SIXP_OUTPUT_STATUS_SUCCESS &&
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                            &cell_list, &cell_list_len,
                            body, body_len) == 0 &&
     (nbr = sixp_nbr_find(dest_addr)) != NULL) {
    add_links_to_schedule(dest_addr, LINK_OPTION_RX,
                          cell_list, cell_list_len);
  }
}

static void
delete_response_sent_callback(void *arg, uint16_t arg_len,
                              const linkaddr_t *dest_addr,
                              sixp_output_status_t status)
{
  uint8_t *body = (uint8_t *)arg;
  uint16_t body_len = arg_len;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  sixp_nbr_t *nbr;

  assert(body != NULL && dest_addr != NULL);

  if(status == SIXP_OUTPUT_STATUS_SUCCESS &&
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                            &cell_list, &cell_list_len,
                            body, body_len) == 0 &&
     (nbr = sixp_nbr_find(dest_addr)) != NULL) {
       child_node *node;
       node = find_child(dest_addr);
       if(node){
         child_list_remove_child(node);
       }
      // print_child_list();
    remove_links_to_schedule(cell_list, cell_list_len);
  }
}

static void
realocate_response_sent_callback(void *arg, uint16_t arg_len,
                           const linkaddr_t *dest_addr,
                           sixp_output_status_t status)
{
  uint8_t *body = (uint8_t *)arg;
  uint16_t body_len = arg_len;

  const uint8_t *cell_list;
  uint16_t cell_list_len;
  
  sixp_nbr_t *nbr;
  sf_simple_cell_t cell;
  child_node *node;
  assert(body != NULL && dest_addr != NULL);

  if(status == SIXP_OUTPUT_STATUS_SUCCESS &&
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                            &cell_list, &cell_list_len,
                            body, body_len) == 0 &&
                            (nbr = sixp_nbr_find(dest_addr)) != NULL) {
     LOG_INFO("sf-simple: 6P realocate Response callback with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        LOG_INFO("\n");
        add_links_to_schedule(&linkaddr_node_addr, LINK_OPTION_TX,(const uint8_t *)(cell_list+sizeof(sf_simple_cell_t)), sizeof(sf_simple_cell_t));
        read_cell((const uint8_t *)(cell_list+sizeof(sf_simple_cell_t)), &cell);
        LOG_INFO("sf-simple: realocate to slot_offset: %d ,channel_offset %d",cell.timeslot_offset,cell.channel_offset);
        node = find_child(&linkaddr_node_addr);
        if(node){
          child_list_set_child_offsets(node,cell.timeslot_offset,cell.channel_offset);
        }
        read_cell((const uint8_t *)cell_list, &cell);
        LOG_INFO(" frome slot_offset: %d ,channel_offset %d\n",cell.timeslot_offset,cell.channel_offset);
        if(!slot_is_used(cell.timeslot_offset)){
          remove_links_to_schedule((const uint8_t *)cell_list, sizeof(sf_simple_cell_t));
        }
        LOG_INFO("sf-simple:node  slot_offset: %d ,channel_offset %d\n",node->slot_offset,node->channel_offset);
  }
  
}


static void
add_req_input(const uint8_t *body, uint16_t body_len, const linkaddr_t *peer_addr)
{
  uint8_t i;
  sf_simple_cell_t cell;
  struct tsch_slotframe *slotframe;
  int feasible_link;
  uint8_t num_cells;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  uint16_t res_len;

  assert(body != NULL && peer_addr != NULL);

  if(sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            &num_cells,
                            body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            &cell_list, &cell_list_len,
                            body, body_len) != 0) {
    LOG_INFO("sf-simple: Parse error on add request\n");
    return;
  }

  LOG_INFO("sf-simple: Received a 6P Add Request for %d links from node %d with LinkList : ",
         num_cells, peer_addr->u8[7]);
  print_cell_list(cell_list, cell_list_len);
  LOG_INFO("\n");

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  if(num_cells > 0 && cell_list_len > 0) {
    memset(res_storage, 0, sizeof(res_storage));
    res_len = 0;

    /* checking availability for requested slots */
    for(i = 0, feasible_link = 0;
        i < cell_list_len && feasible_link < num_cells;
        i += sizeof(cell)) {
      read_cell(&cell_list[i], &cell);
      if(tsch_schedule_get_link_by_timeslot(slotframe,
                                            cell.timeslot_offset) == NULL) {
        sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (uint8_t *)&cell, sizeof(cell),
                               feasible_link,
                               res_storage, sizeof(res_storage));
        res_len += sizeof(cell);
        feasible_link++;
      }
    }

    if(feasible_link == num_cells) {
      /* Links are feasible. Create Link Response packet */
      LOG_INFO("sf-simple: Send a 6P Response to node %d\n", peer_addr->u8[7]);
      sixp_output(SIXP_PKT_TYPE_RESPONSE,
                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                  SF_SIMPLE_SFID,
                  res_storage, res_len, peer_addr,
                  add_response_sent_callback, res_storage, res_len);
    }
  }
}

static void
delete_req_input(const uint8_t *body, uint16_t body_len,
                 const linkaddr_t *peer_addr)
{
  uint8_t i;
  sf_simple_cell_t cell;
  struct tsch_slotframe *slotframe;
  uint8_t num_cells;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  uint16_t res_len;
  int removed_link;

  assert(body != NULL && peer_addr != NULL);

  if(sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            &num_cells,
                            body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            &cell_list, &cell_list_len,
                            body, body_len) != 0) {
    LOG_INFO("sf-simple: Parse error on delete request\n");
    return;
  }

  LOG_INFO("sf-simple: Received a 6P Delete Request for %d links from node %d with LinkList : ",
         num_cells, peer_addr->u8[7]);
  print_cell_list(cell_list, cell_list_len);
  LOG_INFO("\n");

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  memset(res_storage, 0, sizeof(res_storage));
  res_len = 0;

  if(num_cells > 0 && cell_list_len > 0) {
    /* ensure before delete */
    for(i = 0, removed_link = 0; i < (cell_list_len / sizeof(cell)); i++) {
      read_cell(&cell_list[i], &cell);
      if(tsch_schedule_get_link_by_timeslot(slotframe,
                                            cell.timeslot_offset) != NULL) {
        sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (uint8_t *)&cell, sizeof(cell),
                               removed_link,
                               res_storage, sizeof(res_storage));
        res_len += sizeof(cell);
      }
    }
  }
  LOG_INFO("sf-simple:%d requested slots found\n",res_len);
  if(res_len == 0){
    LOG_INFO("sf-simple:send a fake packet back\n");
    cell.timeslot_offset= 0xffff;
    cell.channel_offset= slotframe_handle;
    sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (uint8_t *)&cell, sizeof(cell),
                               0,
                               res_storage, sizeof(res_storage));
    res_len += sizeof(cell);
  }
  /* Links are feasible. Create Link Response packet */
  LOG_INFO("sf-simple: Send a 6P Response to node %d\n", peer_addr->u8[7]);
  sixp_output(SIXP_PKT_TYPE_RESPONSE,
              (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
              SF_SIMPLE_SFID,
              res_storage, res_len, peer_addr,
              delete_response_sent_callback, res_storage, res_len);
}

static void
realocate_req_input(const uint8_t *body, uint16_t body_len, const linkaddr_t *peer_addr)
{
  LOG_INFO("sf-simple: Get realocate request\n");
  uint8_t i;
  sf_simple_cell_t cell;
  struct tsch_slotframe *slotframe;
  int feasible_link = 0;
  uint8_t num_cells;
  
  const uint8_t *rel_cell;
  uint16_t rel_cell_len;

  const uint8_t *cell_list;
  uint16_t cell_list_len;
  uint16_t res_len;

  sf_simple_cell_t res_cell_list[SF_SIMPLE_MAX_LINKS];
  

  assert(body != NULL && peer_addr != NULL);

  if(sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            &num_cells,
                            body, body_len) != 0 ||
     sixp_pkt_get_rel_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            &rel_cell, &rel_cell_len,
                            body, body_len) != 0 ||
     sixp_pkt_get_cand_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            &cell_list, &cell_list_len,
                            body, body_len) != 0) {
    LOG_INFO("sf-simple: Parse error on realocate request\n");
    return;
  }

  LOG_INFO("sf-simple: Received a 6P realocate Request for %d links from node %d ,to realocate ",
         num_cells, peer_addr->u8[7]);
  print_cell_list(rel_cell, rel_cell_len);
  LOG_INFO("with LinkList :");
  print_cell_list(cell_list, cell_list_len);
  LOG_INFO("\n");

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }
  
  if(num_cells > 0 && cell_list_len > 0) {
    memset(res_storage, 0, sizeof(res_storage));
    res_len = sizeof(cell);
    read_cell(&rel_cell[0], &cell);
    LOG_INFO("rel cell:%d\n",cell.timeslot_offset);
    res_cell_list[feasible_link].timeslot_offset = cell.timeslot_offset;
    res_cell_list[feasible_link].channel_offset = cell.channel_offset;
    feasible_link++;
    /* checking availability for requested slots */
    for(i = 0, feasible_link = 1;
        i < cell_list_len && feasible_link < num_cells+1;
        i += sizeof(cell)) {
      read_cell(&cell_list[i], &cell);
      LOG_INFO("cell:%d\n",cell.timeslot_offset);
      if(tsch_schedule_get_link_by_timeslot(slotframe,
                                            cell.timeslot_offset) == NULL 
                                            && !slot_is_used(cell.timeslot_offset)
                                            && cell.timeslot_offset != self_default_slot
                                            && cell.timeslot_offset<SF_CONF_SIX_TOP_SLOTFRAME_LENGTH) {
        res_cell_list[feasible_link].timeslot_offset = cell.timeslot_offset;
        res_cell_list[feasible_link].channel_offset = cell.channel_offset;
        res_len += sizeof(cell);
        feasible_link++;
      }
    }
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (const uint8_t *)res_cell_list, feasible_link * sizeof(sf_simple_cell_t),
                               0,
                               res_storage, sizeof(res_storage));

    if(feasible_link == num_cells+1) {
      /* Links are feasible. Create Link Response packet */
      LOG_INFO("sf-simple: Send a 6P Response to node %d\n", peer_addr->u8[7]);
      sixp_output(SIXP_PKT_TYPE_RESPONSE,
                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                  SF_SIMPLE_SFID,
                  res_storage, res_len, peer_addr,
                  realocate_response_sent_callback, res_storage, res_len);
    LOG_INFO("with LinkList :");
    print_cell_list((const uint8_t *)res_cell_list, feasible_link * sizeof(sf_simple_cell_t));
    }
  }
}

static void
input(sixp_pkt_type_t type, sixp_pkt_code_t code,
      const uint8_t *body, uint16_t body_len, const linkaddr_t *src_addr)
{
  assert(body != NULL && body != NULL);
  switch(type) {
    case SIXP_PKT_TYPE_REQUEST:
      request_input(code.cmd, body, body_len, src_addr);
      break;
    case SIXP_PKT_TYPE_RESPONSE:
      response_input(code.cmd, body, body_len, src_addr);
      break;
    default:
      /* unsupported */
      break;
  }
}

static void
request_input(sixp_pkt_cmd_t cmd,
              const uint8_t *body, uint16_t body_len,
              const linkaddr_t *peer_addr)
{
  assert(body != NULL && peer_addr != NULL);

  switch(cmd) {
    case SIXP_PKT_CMD_ADD:
      add_req_input(body, body_len, peer_addr);
      break;
    case SIXP_PKT_CMD_DELETE:
      delete_req_input(body, body_len, peer_addr);
      break;
    case SIXP_PKT_CMD_RELOCATE:
      realocate_req_input(body, body_len, peer_addr);
      break;
    default:
      /* unsupported request */
      break;
  }
}
static void
response_input(sixp_pkt_rc_t rc,
               const uint8_t *body, uint16_t body_len,
               const linkaddr_t *peer_addr)
{
  const uint8_t *cell_list;
  uint16_t cell_list_len;

  sf_simple_cell_t cell;

  sixp_nbr_t *nbr;
  sixp_trans_t *trans;
  child_node *node;
  assert(body != NULL && peer_addr != NULL);

  if((nbr = sixp_nbr_find(peer_addr)) == NULL ||
     (trans = sixp_trans_find(peer_addr)) == NULL) {
    return;
  }

  if(rc == SIXP_PKT_RC_SUCCESS) {
    switch(sixp_trans_get_cmd(trans)) {
      case SIXP_PKT_CMD_ADD:
        if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                                  &cell_list, &cell_list_len,
                                  body, body_len) != 0) {
          LOG_INFO("sf-simple: Parse error on add response\n");
          return;
        }
        LOG_INFO("sf-simple: Received a 6P Add Response with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        LOG_INFO("\n");
        add_links_to_schedule(peer_addr, LINK_OPTION_TX,
                              cell_list, cell_list_len);
        break;
      case SIXP_PKT_CMD_DELETE:
        if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                                  &cell_list, &cell_list_len,
                                  body, body_len) != 0) {
          LOG_INFO("sf-simple: Parse error on add response\n");
          return;
        }
        LOG_INFO("sf-simple: Received a 6P Delete Response with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        LOG_INFO("\n");
        read_cell(&cell_list[0], &cell);
         if(!slot_is_used(cell.timeslot_offset)){
          remove_links_to_schedule(cell_list, cell_list_len);
         }
        process_post(&sf_wait_parent_switch_done_process,sf_parent_switch_done, NULL);
        break;
      case SIXP_PKT_CMD_RELOCATE:
         if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                                  &cell_list, &cell_list_len,
                                  body, body_len) != 0) {
          LOG_INFO("sf-simple: Parse error on realocate response\n");
          return;
        }
        LOG_INFO("sf-simple: Received a 6P realocate Response with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        LOG_INFO("\n");
        node = find_child(peer_addr);
        if(node){
        
        add_links_to_schedule(peer_addr, LINK_OPTION_RX,(const uint8_t *)(cell_list+sizeof(sf_simple_cell_t)), sizeof(sf_simple_cell_t));
        read_cell((const uint8_t *)(cell_list+sizeof(sf_simple_cell_t)), &cell);
        LOG_INFO("sf-simple: realocate to slot_offset: %d ,channel_offset %d",cell.timeslot_offset,cell.channel_offset);
        child_list_set_child_offsets(node,cell.timeslot_offset,cell.channel_offset);
        read_cell((const uint8_t *)cell_list, &cell);
        LOG_INFO(" frome slot_offset: %d ,channel_offset %d\n",cell.timeslot_offset,cell.channel_offset);
        if(!exclude_lladdr_slot_is_used(peer_addr,cell.timeslot_offset)){
          remove_links_to_schedule((const uint8_t *)cell_list, sizeof(sf_simple_cell_t));
          LOG_INFO("sf-simple:node  slot_offset: %d ,channel_offset %d\n",node->slot_offset,node->channel_offset);
        }
        //print_child_list();
        }
        break;
      case SIXP_PKT_CMD_COUNT:
      case SIXP_PKT_CMD_LIST:
      case SIXP_PKT_CMD_CLEAR:
      default:
        LOG_INFO("sf-simple: unsupported response\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Initiates a Sixtop Link addition
 */
int
sf_simple_add_links(linkaddr_t *peer_addr, uint8_t num_links)
{
  uint8_t i = 0, index = 0;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);

  uint8_t req_len;
  sf_simple_cell_t cell_list[SF_SIMPLE_MAX_LINKS];

  /* Flag to prevent repeated slots */
  uint8_t slot_check = 1;
  uint16_t random_slot = 0;

  assert(peer_addr != NULL && sf != NULL);

  do {
    /* Randomly select a slot offset within SF_SIX_TOP_SLOTFRAME_LENGTH */
    random_slot = ((random_rand() & 0xFF)) % SF_SIX_TOP_SLOTFRAME_LENGTH;

    if(tsch_schedule_get_link_by_timeslot(sf, random_slot) == NULL) {

      /* To prevent repeated slots */
      for(i = 0; i < index; i++) {
        if(cell_list[i].timeslot_offset != random_slot) {
          /* Random selection resulted in a free slot */
          if(i == index - 1) { /* Checked till last index of link list */
            slot_check = 1;
            break;
          }
        } else {
          /* Slot already present in CandidateLinkList */
          slot_check++;
          break;
        }
      }

      /* Random selection resulted in a free slot, add it to linklist */
      if(slot_check == 1) {
        cell_list[index].timeslot_offset = random_slot;
        cell_list[index].channel_offset = 0;

        index++;
        slot_check++;
      } else if(slot_check > SF_SIX_TOP_SLOTFRAME_LENGTH) {
        LOG_INFO("sf-simple:! Number of trials for free slot exceeded...\n");
        return -1;
        break; /* exit while loop */
      }
    }
  } while(index < SF_SIMPLE_MAX_LINKS);

  /* Create a Sixtop Add Request. Return 0 if Success */
  if(index == 0 ) {
    return -1;
  }

  memset(req_storage, 0, sizeof(req_storage));
  if(sixp_pkt_set_cell_options(SIXP_PKT_TYPE_REQUEST,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                               SIXP_PKT_CELL_OPTION_TX,
                               req_storage,
                               sizeof(req_storage)) != 0 ||
     sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            num_links,
                            req_storage,
                            sizeof(req_storage)) != 0 ||
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            (const uint8_t *)cell_list,
                            index * sizeof(sf_simple_cell_t), 0,
                            req_storage, sizeof(req_storage)) != 0) {
    LOG_INFO("sf-simple: Build error on add request\n");
    return -1;
  }

  /* The length of fixed part is 4 bytes: Metadata, CellOptions, and NumCells */
  req_len = 4 + index * sizeof(sf_simple_cell_t);
  sixp_output(SIXP_PKT_TYPE_REQUEST, (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
              SF_SIMPLE_SFID,
              req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  LOG_INFO("sf-simple: Send a 6P Add Request for %d links to node %d with LinkList : ",
         num_links, peer_addr->u8[7]);
  print_cell_list((const uint8_t *)cell_list, index * sizeof(sf_simple_cell_t));
  LOG_INFO("\n");

  return 0;
}
/*---------------------------------------------------------------------------*/
/* Initiates a Sixtop Link deletion
 */
int
sf_simple_remove_links(linkaddr_t *peer_addr)
{if(peer_addr == NULL){
 LOG_INFO("sf-simple: peer_addrs is NULL\n");
}
else
{
  LOG_INFO("sf-simple: Prepare to remove frome");
  LOG_INFO_LLADDR(peer_addr);
  LOG_INFO("\n");
}
  uint8_t i = 0, index = 0;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  struct tsch_link *l;

  uint16_t req_len;
  sf_simple_cell_t cell;

 if(sf == NULL){
  LOG_INFO("sf-simple: sf is NULL\n");
  }
  assert(peer_addr != NULL && sf != NULL);

  for(i = 0; i < SF_SIX_TOP_SLOTFRAME_LENGTH; i++) {
    l = tsch_schedule_get_link_by_timeslot(sf, i);

    if(l) {
       LOG_INFO("sf-simple:Get link %d :",i);
         LOG_INFO_LLADDR(&l->addr);
        LOG_INFO("\n");
      /* Non-zero value indicates a scheduled link */
      if((l->link_options | LINK_OPTION_TX)) {
        /* This link is scheduled as a TX link to the specified neighbor */
        cell.timeslot_offset = i;
        cell.channel_offset = l->channel_offset;
        index++;
        break;   /* delete atmost one */
      }
    }
  }

  if(index == 0) {
     LOG_INFO("sf-simple: nothing to remove\n");
    return -1;
  }

  memset(req_storage, 0, sizeof(req_storage));
  if(sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            1,
                            req_storage,
                            sizeof(req_storage)) != 0 ||
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            (const uint8_t *)&cell, sizeof(cell),
                            0,
                            req_storage, sizeof(req_storage)) != 0) {
    LOG_INFO("sf-simple: Build error on add request\n");
    return -1;
  }
  /* The length of fixed part is 4 bytes: Metadata, CellOptions, and NumCells */
  req_len = 4 + sizeof(sf_simple_cell_t);

  sixp_output(SIXP_PKT_TYPE_REQUEST, (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
              SF_SIMPLE_SFID,
              req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  LOG_INFO("sf-simple: Send a 6P Delete Request for %d links to node %d with LinkList : ",
         1, peer_addr->u8[7]);
  print_cell_list((const uint8_t *)&cell, sizeof(cell));
  LOG_INFO("\n");

  return 0;
}
/*------------------------------------------------------------*/

struct realocate_process_data_s{
  uint16_t timeslot;
  uint16_t channel;
} realocate_process_data = {-1,-1};

int sf_simple_realocate_links(linkaddr_t *peer_addr,uint16_t timeslot,uint16_t channel)
{
uint8_t i = 0, index = 0;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
LOG_INFO("sf-simple: Prepare to realocate\n");
  uint8_t req_len;
  sf_simple_cell_t rel_cell;
  rel_cell.timeslot_offset=timeslot;
  rel_cell.channel_offset=channel;
  sf_simple_cell_t cell_list[SF_SIMPLE_MAX_LINKS];

  /* Flag to prevent repeated slots */
  uint8_t slot_check = 1;
  uint16_t random_slot = 0;

  
  assert(peer_addr != NULL && sf != NULL);

  do {
    /* Randomly select a slot offset within SF_SIX_TOP_SLOTFRAME_LENGTH */
    random_slot = ((random_rand() & 0xFF)) % SF_SIX_TOP_SLOTFRAME_LENGTH;

    if(tsch_schedule_get_link_by_timeslot(sf, random_slot) == NULL && !slot_is_used(random_slot) && random_slot != self_default_slot) {

      /* To prevent repeated slots */
      for(i = 0; i < index; i++) {
        if(cell_list[i].timeslot_offset != random_slot) {
          /* Random selection resulted in a free slot */
          if(i == index - 1) { /* Checked till last index of link list */
            slot_check = 1;
            break;
          }
        } else {
          /* Slot already present in CandidateLinkList */
          slot_check++;
          break;
        }
      }

      /* Random selection resulted in a free slot, add it to linklist */
      if(slot_check == 1) {
        cell_list[index].timeslot_offset = random_slot;
        cell_list[index].channel_offset = slotframe_handle;
  LOG_INFO("sf-simple:find %d %d %d\n",cell_list[index].timeslot_offset,cell_list[index].channel_offset,index);
        index++;
        slot_check++;
      } else if(slot_check > SF_SIX_TOP_SLOTFRAME_LENGTH) {
        LOG_INFO("sf-simple:! Number of trials for free slot exceeded...\n");
        return -1;
        break; /* exit while loop */
      }
    }
  } while(index < SF_SIMPLE_MAX_LINKS-1);
  LOG_INFO("sf-simple: index %d\n",index);

  

  /* Create a Sixtop Add Request. Return 0 if Success */
  if(index == 0 ) {
    return -1;
  }
  
  memset(req_storage, 0, sizeof(req_storage));
  print_cell_list((const uint8_t *)cell_list, index * sizeof(sf_simple_cell_t));
  if(sixp_pkt_set_cell_options(SIXP_PKT_TYPE_REQUEST,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                               SIXP_PKT_CELL_OPTION_TX,
                               req_storage,
                               sizeof(req_storage)) != 0 ||
     sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            1,
                            req_storage,
                            sizeof(req_storage)) != 0 ||
     sixp_pkt_set_rel_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            (const uint8_t *)&rel_cell,
                            sizeof(sf_simple_cell_t), 0,
                            req_storage,
                            sizeof(req_storage)) != 0||
     sixp_pkt_set_cand_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
                            (const uint8_t *)cell_list,
                            index * sizeof(sf_simple_cell_t), 0,
                            req_storage,
                            sizeof(req_storage)) != 0) {
    LOG_INFO("sf-simple: Build error on add request\n");
    return -1;
  }

    /* The length of fixed part is 4 bytes: Metadata, CellOptions, and NumCells 
      plus rel_cell & cand_cells
  */
  req_len = 4 + (1+index) * sizeof(sf_simple_cell_t);
  sixp_output(SIXP_PKT_TYPE_REQUEST, (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_RELOCATE,
              SF_SIMPLE_SFID,
              req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  LOG_INFO("sf-simple: Send a 6P realocate Request for %d links to node %d ,to realocate ",
         1, peer_addr->u8[7]);
  print_cell_list((const uint8_t *)&rel_cell,sizeof(sf_simple_cell_t));
  LOG_INFO("with LinkList :");
  print_cell_list((const uint8_t *)cell_list, index * sizeof(sf_simple_cell_t));
  realocate_process_data.timeslot = timeslot;
  realocate_process_data.channel = channel;
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Initiates a Sixtop Link deletion
 */
int
sf_simple_remove_direct_link(linkaddr_t *peer_addr,uint16_t timeslot)
{if(peer_addr == NULL){
 LOG_INFO("sf-simple: peer_addrs is NULL\n");
}
else
{
  LOG_INFO("sf-simple: Prepare to remove frome");
  LOG_INFO_LLADDR(peer_addr);
  LOG_INFO("\n");
}
  
  uint16_t req_len;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  struct tsch_link *l;

 l=tsch_schedule_get_link_by_timeslot(sf,timeslot);

if(l==NULL){
  LOG_INFO("sf-simple: link not found");
  return -1;
}
  sf_simple_cell_t cell;
  cell.timeslot_offset=timeslot;
  cell.channel_offset=l->channel_offset;

  memset(req_storage, 0, sizeof(req_storage));
  if(sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            1,
                            req_storage,
                            sizeof(req_storage)) != 0 ||
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            (const uint8_t *)&cell, sizeof(cell),
                            0,
                            req_storage, sizeof(req_storage)) != 0) {
    LOG_INFO("sf-simple: Build error on add request\n");
    return -1;
  }
  /* The length of fixed part is 4 bytes: Metadata, CellOptions, and NumCells */
  req_len = 4 + sizeof(sf_simple_cell_t);

  sixp_output(SIXP_PKT_TYPE_REQUEST, (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
              SF_SIMPLE_SFID,
              req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  LOG_INFO("sf-simple: Send a 6P Delete Request for %d links to node %d with LinkList : ",
         1, peer_addr->u8[7]);
  print_cell_list((const uint8_t *)&cell, sizeof(cell));
  LOG_INFO("\n");

  return 0;
}

typedef struct {
  uint16_t default_tx_timeslot;
} parent_switch_process_data;

void
sf_simple_switching_parent_callback(linkaddr_t *old_addr, linkaddr_t *new_addr,uint16_t default_tx_timeslot)
{
  LOG_INFO("in sf_simple_switching_parent_callback\n");
  child_node *node;
  node = find_child(&linkaddr_node_addr);

  if(node && sf_simple_remove_direct_link(old_addr,node->slot_offset) == 0){
    LOG_INFO("Add to new parent success\n");
    parent_switch_process_data data = {default_tx_timeslot};
    process_start(&sf_wait_parent_switch_done_process, &data);
    
  }

}

void realocate_faild_hack(const linkaddr_t *peer_addr,uint16_t timeslot,uint16_t channel){
struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  struct tsch_link *l;
  uint8_t link_options = LINK_OPTION_RX;
  l=tsch_schedule_get_link_by_timeslot(sf,timeslot);
  if(l){
   link_options |= l->link_options;
  }
  child_node *node;
    node = find_child(peer_addr);
   child_list_set_child_offsets(node,timeslot,channel);
     /* Add/update link */
      tsch_schedule_add_link(sf, link_options, LINK_TYPE_NORMAL, &tsch_broadcast_address,
          timeslot, channel);
}

typedef struct {
  sixp_pkt_cmd_t cmd;
  const linkaddr_t *peer_addr;
} process_data;


static void
timeout(sixp_pkt_cmd_t cmd, const linkaddr_t *peer_addr)
{
  LOG_INFO("transaction timeout\n");
  process_data data_to_process = {cmd, peer_addr};
  process_start(&sf_wait_for_retry_process, &data_to_process);
}


PROCESS_THREAD(sf_wait_for_retry_process, ev, data)
{
  static struct etimer et;
  static sixp_pkt_cmd_t cmd = 0x00;
  static const linkaddr_t *peer_addr_c = NULL;
  static linkaddr_t peer_addr; //const problem
  uint8_t random_time = (((random_rand() & 0xFF)) % TIMEOUT_WAIT_FOR_RETRY_RANDOM)+3;
  child_node *node;
  PROCESS_BEGIN();
  
  etimer_set(&et, CLOCK_SECOND*random_time);
  cmd = ((process_data *)data)->cmd;
  peer_addr_c = ((process_data *)data)->peer_addr;
  peer_addr = *peer_addr_c;
  LOG_INFO("in sf_wait_for_retry_process cmd=%d node=%d\n", cmd, peer_addr.u8[7]);  

  sixp_trans_t *trans = sixp_trans_find(&peer_addr);
  if(trans != NULL) {
    sixp_trans_state_t state;
    state = sixp_trans_get_state(trans);
    if (state == SIXP_TRANS_STATE_REQUEST_SENT || state == SIXP_TRANS_STATE_RESPONSE_SENT || state == SIXP_TRANS_STATE_CONFIRMATION_SENT) {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
      
      switch (cmd) {
      case SIXP_PKT_CMD_ADD:
        LOG_INFO("Retry add (doing nothing now)\n");
        //sf_simple_add_links(&peer_addr, 1);
        break;
      case SIXP_PKT_CMD_DELETE:
        LOG_INFO("Retry delete\n");
        node = find_child(&linkaddr_node_addr);
        if(node){
        sf_simple_remove_direct_link(&peer_addr,node->slot_offset);
        }
        break;
      case SIXP_PKT_CMD_RELOCATE:
       LOG_INFO("Retry relocate %d %d\n",realocate_process_data.timeslot,realocate_process_data.channel);
       realocate_faild_hack(&peer_addr,realocate_process_data.timeslot,realocate_process_data.channel);
       //sf_simple_realocate_links(&peer_addr,realocate_process_data.timeslot,realocate_process_data.channel);

       // use other method to maintain realocate faild
       break;
      default:
        /* unsupported request */
        break;
      }
    }
  }
    
  etimer_reset(&et);  
  PROCESS_END();
}


PROCESS_THREAD(sf_wait_parent_switch_done_process, ev, data)
{
  static uint16_t default_tx_timeslot;
  static struct etimer et;
  struct tsch_slotframe *slotframe;
  PROCESS_BEGIN();
  etimer_set(&et, CLOCK_SECOND * 1);
  default_tx_timeslot = ((parent_switch_process_data *)data) -> default_tx_timeslot;
  PROCESS_WAIT_EVENT_UNTIL(ev == sf_parent_switch_done);
  PROCESS_YIELD_UNTIL(etimer_expired(&et));
  LOG_INFO("sf_trans_done\n");
  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe != NULL) {
  child_node *node;
    node = find_child(&linkaddr_node_addr);
    if(node != NULL && default_tx_timeslot >= 0){
      child_list_set_child_offsets(node,default_tx_timeslot,slotframe_handle);
      tsch_schedule_add_link(slotframe,
        LINK_OPTION_SHARED | LINK_OPTION_TX,
        LINK_TYPE_NORMAL, &tsch_broadcast_address,
        default_tx_timeslot, slotframe_handle);
    }
  }
  PROCESS_END();
}

void remove_link_not_in_child_list(){
  struct tsch_slotframe *slotframe;
  struct tsch_link *l;
  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  int i;
  for(i=0;i<SF_SIX_TOP_SLOTFRAME_LENGTH;i++){
    l=tsch_schedule_get_link_by_timeslot(slotframe,i);
    if(l && !slot_is_used(l->timeslot)){
      LOG_PRINT("Remove link: Link Options %02x, type %u, timeslot %u, channel offset %u, address %u\n",
      l->link_options, l->link_type, l->timeslot, l->channel_offset, l->addr.u8[7]);
      tsch_schedule_remove_link(slotframe,l);
    }
  }

}


PROCESS_THREAD(sf_link_maintain_and_realocate_retry, ev, data)
{
  static struct etimer etaa;
  PROCESS_BEGIN();
  etimer_set(&etaa, CLOCK_SECOND * SF_LINK_MAINTAIN_PERIOD);
  while(1) {
    PROCESS_YIELD_UNTIL(etimer_expired(&etaa));
    etimer_reset(&etaa);
    LOG_INFO("start maintain\n");
    remove_link_not_in_child_list();
    //retry realocate node in child list with same slot offset
    child_node *node;
    node = find_dupilcate_used_slot();
    if(node){
      sf_simple_realocate_links(&node->address,node->slot_offset,node->channel_offset);
    }
    //print_child_list();
    //tsch_schedule_print();
  }
  PROCESS_END();
}

static void
init(void)
{
  #if SF_SIX_TOP_ENABLE_RELOCATE
  sf_parent_switch_done = process_alloc_event();
  process_start(&sf_link_maintain_and_realocate_retry,NULL);
  #endif
}
const sixtop_sf_t sf_simple_driver = {
  SF_SIMPLE_SFID,
  CLOCK_SECOND*10,
  init,
  input,
  timeout
};
