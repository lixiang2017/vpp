/*
 * snat.c - simple nat plugin
 *
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vnet/ip/ip4.h>
#include <vnet/plugin/plugin.h>
#include <snat/snat.h>
#include <snat/snat_ipfix_logging.h>
#include <snat/snat_det.h>
#include <snat/nat64.h>
#include <vnet/fib/fib_table.h>
#include <vnet/fib/ip4_fib.h>

#include <vpp/app/version.h>

snat_main_t snat_main;


/* Hook up input features */
VNET_FEATURE_INIT (ip4_snat_in2out, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-in2out",
  .runs_before = VNET_FEATURES ("snat-out2in"),
};
VNET_FEATURE_INIT (ip4_snat_out2in, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-out2in",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};
VNET_FEATURE_INIT (ip4_snat_det_in2out, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-det-in2out",
  .runs_before = VNET_FEATURES ("snat-det-out2in"),
};
VNET_FEATURE_INIT (ip4_snat_det_out2in, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-det-out2in",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};
VNET_FEATURE_INIT (ip4_snat_in2out_worker_handoff, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-in2out-worker-handoff",
  .runs_before = VNET_FEATURES ("snat-out2in-worker-handoff"),
};
VNET_FEATURE_INIT (ip4_snat_out2in_worker_handoff, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-out2in-worker-handoff",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};
VNET_FEATURE_INIT (ip4_snat_in2out_fast, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-in2out-fast",
  .runs_before = VNET_FEATURES ("snat-out2in-fast"),
};
VNET_FEATURE_INIT (ip4_snat_out2in_fast, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "snat-out2in-fast",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};

/* *INDENT-OFF* */
VLIB_PLUGIN_REGISTER () = {
    .version = VPP_BUILD_VER,
    .description = "Network Address Translation",
};
/* *INDENT-ON* */

/**
 * @brief Add/del NAT address to FIB.
 *
 * Add the external NAT address to the FIB as receive entries. This ensures
 * that VPP will reply to ARP for this address and we don't need to enable
 * proxy ARP on the outside interface.
 *
 * @param addr IPv4 address.
 * @param plen address prefix length
 * @param sw_if_index Interface.
 * @param is_add If 0 delete, otherwise add.
 */
void
snat_add_del_addr_to_fib (ip4_address_t * addr, u8 p_len, u32 sw_if_index,
                          int is_add)
{
  fib_prefix_t prefix = {
    .fp_len = p_len,
    .fp_proto = FIB_PROTOCOL_IP4,
    .fp_addr = {
        .ip4.as_u32 = addr->as_u32,
    },
  };
  u32 fib_index = ip4_fib_table_get_index_for_sw_if_index(sw_if_index);

  if (is_add)
    fib_table_entry_update_one_path(fib_index,
                                    &prefix,
                                    FIB_SOURCE_PLUGIN_HI,
                                    (FIB_ENTRY_FLAG_CONNECTED |
                                     FIB_ENTRY_FLAG_LOCAL |
                                     FIB_ENTRY_FLAG_EXCLUSIVE),
                                    FIB_PROTOCOL_IP4,
                                    NULL,
                                    sw_if_index,
                                    ~0,
                                    1,
                                    NULL,
                                    FIB_ROUTE_PATH_FLAG_NONE);
  else
    fib_table_entry_delete(fib_index,
                           &prefix,
                           FIB_SOURCE_PLUGIN_HI);
}

void snat_add_address (snat_main_t *sm, ip4_address_t *addr, u32 vrf_id)
{
  snat_address_t * ap;
  snat_interface_t *i;

  if (vrf_id != ~0)
    sm->vrf_mode = 1;

  /* Check if address already exists */
  vec_foreach (ap, sm->addresses)
    {
      if (ap->addr.as_u32 == addr->as_u32)
        return;
    }

  vec_add2 (sm->addresses, ap, 1);
  ap->addr = *addr;
  ap->fib_index = ip4_fib_index_from_table_id(vrf_id);
#define _(N, i, n, s) \
  clib_bitmap_alloc (ap->busy_##n##_port_bitmap, 65535);
  foreach_snat_protocol
#undef _

  /* Add external address to FIB */
  pool_foreach (i, sm->interfaces,
  ({
    if (i->is_inside)
      continue;

    snat_add_del_addr_to_fib(addr, 32, i->sw_if_index, 1);
    break;
  }));
}

static int is_snat_address_used_in_static_mapping (snat_main_t *sm,
                                                   ip4_address_t addr)
{
  snat_static_mapping_t *m;
  pool_foreach (m, sm->static_mappings,
  ({
      if (m->external_addr.as_u32 == addr.as_u32)
        return 1;
  }));

  return 0;
}

void increment_v4_address (ip4_address_t * a)
{
  u32 v;
  
  v = clib_net_to_host_u32(a->as_u32) + 1;
  a->as_u32 = clib_host_to_net_u32(v);
}

static void 
snat_add_static_mapping_when_resolved (snat_main_t * sm, 
                                       ip4_address_t l_addr, 
                                       u16 l_port, 
                                       u32 sw_if_index, 
                                       u16 e_port, 
                                       u32 vrf_id,
                                       snat_protocol_t proto,
                                       int addr_only,  
                                       int is_add)
{
  snat_static_map_resolve_t *rp;

  vec_add2 (sm->to_resolve, rp, 1);
  rp->l_addr.as_u32 = l_addr.as_u32;
  rp->l_port = l_port;
  rp->sw_if_index = sw_if_index;
  rp->e_port = e_port;
  rp->vrf_id = vrf_id;
  rp->proto = proto;
  rp->addr_only = addr_only;
  rp->is_add = is_add;
}

/**
 * @brief Add static mapping.
 *
 * Create static mapping between local addr+port and external addr+port.
 *
 * @param l_addr Local IPv4 address.
 * @param e_addr External IPv4 address.
 * @param l_port Local port number.
 * @param e_port External port number.
 * @param vrf_id VRF ID.
 * @param addr_only If 0 address port and pair mapping, otherwise address only.
 * @param sw_if_index External port instead of specific IP address.
 * @param is_add If 0 delete static mapping, otherwise add.
 *
 * @returns
 */
int snat_add_static_mapping(ip4_address_t l_addr, ip4_address_t e_addr,
                            u16 l_port, u16 e_port, u32 vrf_id, int addr_only,
                            u32 sw_if_index, snat_protocol_t proto, int is_add)
{
  snat_main_t * sm = &snat_main;
  snat_static_mapping_t *m;
  snat_session_key_t m_key;
  clib_bihash_kv_8_8_t kv, value;
  snat_address_t *a = 0;
  u32 fib_index = ~0;
  uword * p;
  snat_interface_t *interface;
  int i;

  /* If the external address is a specific interface address */
  if (sw_if_index != ~0)
    {
      ip4_address_t * first_int_addr;

      /* Might be already set... */
      first_int_addr = ip4_interface_first_address 
        (sm->ip4_main, sw_if_index, 0 /* just want the address*/);

      /* DHCP resolution required? */
      if (first_int_addr == 0)
        {
          snat_add_static_mapping_when_resolved 
            (sm, l_addr, l_port, sw_if_index, e_port, vrf_id, proto,
             addr_only,  is_add);
          return 0;
        }
        else
          e_addr.as_u32 = first_int_addr->as_u32;
    }

  m_key.addr = e_addr;
  m_key.port = addr_only ? 0 : e_port;
  m_key.protocol = addr_only ? 0 : proto;
  m_key.fib_index = sm->outside_fib_index;
  kv.key = m_key.as_u64;
  if (clib_bihash_search_8_8 (&sm->static_mapping_by_external, &kv, &value))
    m = 0;
  else
    m = pool_elt_at_index (sm->static_mappings, value.value);

  if (is_add)
    {
      if (m)
        return VNET_API_ERROR_VALUE_EXIST;

      /* Convert VRF id to FIB index */
      if (vrf_id != ~0)
        {
          p = hash_get (sm->ip4_main->fib_index_by_table_id, vrf_id);
          if (!p)
            return VNET_API_ERROR_NO_SUCH_FIB;
          fib_index = p[0];
        }
      /* If not specified use inside VRF id from SNAT plugin startup config */
      else
        {
          fib_index = sm->inside_fib_index;
          vrf_id = sm->inside_vrf_id;
        }

      /* Find external address in allocated addresses and reserve port for
         address and port pair mapping when dynamic translations enabled */
      if (!addr_only && !(sm->static_mapping_only))
        {
          for (i = 0; i < vec_len (sm->addresses); i++)
            {
              if (sm->addresses[i].addr.as_u32 == e_addr.as_u32)
                {
                  a = sm->addresses + i;
                  /* External port must be unused */
                  switch (proto)
                    {
#define _(N, j, n, s) \
                    case SNAT_PROTOCOL_##N: \
                      if (clib_bitmap_get_no_check (a->busy_##n##_port_bitmap, e_port)) \
                        return VNET_API_ERROR_INVALID_VALUE; \
                      clib_bitmap_set_no_check (a->busy_##n##_port_bitmap, e_port, 1); \
                      if (e_port > 1024) \
                        a->busy_##n##_ports++; \
                      break;
                      foreach_snat_protocol
#undef _
                    default:
                      clib_warning("unknown_protocol");
                      return VNET_API_ERROR_INVALID_VALUE_2;
                    }
                  break;
                }
            }
          /* External address must be allocated */
          if (!a)
            return VNET_API_ERROR_NO_SUCH_ENTRY;
        }

      pool_get (sm->static_mappings, m);
      memset (m, 0, sizeof (*m));
      m->local_addr = l_addr;
      m->external_addr = e_addr;
      m->addr_only = addr_only;
      m->vrf_id = vrf_id;
      m->fib_index = fib_index;
      if (!addr_only)
        {
          m->local_port = l_port;
          m->external_port = e_port;
          m->proto = proto;
        }

      m_key.addr = m->local_addr;
      m_key.port = m->local_port;
      m_key.protocol = m->proto;
      m_key.fib_index = m->fib_index;
      kv.key = m_key.as_u64;
      kv.value = m - sm->static_mappings;
      clib_bihash_add_del_8_8(&sm->static_mapping_by_local, &kv, 1);

      m_key.addr = m->external_addr;
      m_key.port = m->external_port;
      m_key.fib_index = sm->outside_fib_index;
      kv.key = m_key.as_u64;
      kv.value = m - sm->static_mappings;
      clib_bihash_add_del_8_8(&sm->static_mapping_by_external, &kv, 1);

      /* Assign worker */
      if (sm->workers)
        {
          snat_user_key_t w_key0;
          snat_worker_key_t w_key1;

          w_key0.addr = m->local_addr;
          w_key0.fib_index = m->fib_index;
          kv.key = w_key0.as_u64;

          if (clib_bihash_search_8_8 (&sm->worker_by_in, &kv, &value))
            {
              kv.value = sm->first_worker_index +
                sm->workers[sm->next_worker++ % vec_len (sm->workers)];

              clib_bihash_add_del_8_8 (&sm->worker_by_in, &kv, 1);
            }
          else
            {
              kv.value = value.value;
            }

          w_key1.addr = m->external_addr;
          w_key1.port = clib_host_to_net_u16 (m->external_port);
          w_key1.fib_index = sm->outside_fib_index;
          kv.key = w_key1.as_u64;
          clib_bihash_add_del_8_8 (&sm->worker_by_out, &kv, 1);
        }
    }
  else
    {
      if (!m)
        return VNET_API_ERROR_NO_SUCH_ENTRY;

      /* Free external address port */
      if (!addr_only && !(sm->static_mapping_only))
        {
          for (i = 0; i < vec_len (sm->addresses); i++)
            {
              if (sm->addresses[i].addr.as_u32 == e_addr.as_u32)
                {
                  a = sm->addresses + i;
                  switch (proto)
                    {
#define _(N, j, n, s) \
                    case SNAT_PROTOCOL_##N: \
                      clib_bitmap_set_no_check (a->busy_##n##_port_bitmap, e_port, 0); \
                      if (e_port > 1024) \
                        a->busy_##n##_ports--; \
                      break;
                      foreach_snat_protocol
#undef _
                    default:
                      clib_warning("unknown_protocol");
                      return VNET_API_ERROR_INVALID_VALUE_2;
                    }
                  break;
                }
            }
        }

      m_key.addr = m->local_addr;
      m_key.port = m->local_port;
      m_key.protocol = m->proto;
      m_key.fib_index = m->fib_index;
      kv.key = m_key.as_u64;
      clib_bihash_add_del_8_8(&sm->static_mapping_by_local, &kv, 0);

      m_key.addr = m->external_addr;
      m_key.port = m->external_port;
      m_key.fib_index = sm->outside_fib_index;
      kv.key = m_key.as_u64;
      clib_bihash_add_del_8_8(&sm->static_mapping_by_external, &kv, 0);

      /* Delete session(s) for static mapping if exist */
      if (!(sm->static_mapping_only) ||
          (sm->static_mapping_only && sm->static_mapping_connection_tracking))
        {
          snat_user_key_t u_key;
          snat_user_t *u;
          dlist_elt_t * head, * elt;
          u32 elt_index, head_index, del_elt_index;
          u32 ses_index;
          u64 user_index;
          snat_session_t * s;
          snat_main_per_thread_data_t *tsm;

          u_key.addr = m->local_addr;
          u_key.fib_index = m->fib_index;
          kv.key = u_key.as_u64;
          if (!clib_bihash_search_8_8 (&sm->user_hash, &kv, &value))
            {
              user_index = value.value;
              if (!clib_bihash_search_8_8 (&sm->worker_by_in, &kv, &value))
                tsm = vec_elt_at_index (sm->per_thread_data, value.value);
              else
                tsm = vec_elt_at_index (sm->per_thread_data, sm->num_workers);
              u = pool_elt_at_index (tsm->users, user_index);
              if (u->nstaticsessions)
                {
                  head_index = u->sessions_per_user_list_head_index;
                  head = pool_elt_at_index (tsm->list_pool, head_index);
                  elt_index = head->next;
                  elt = pool_elt_at_index (tsm->list_pool, elt_index);
                  ses_index = elt->value;
                  while (ses_index != ~0)
                    {
                      s =  pool_elt_at_index (tsm->sessions, ses_index);
                      del_elt_index = elt_index;
                      elt_index = elt->next;
                      elt = pool_elt_at_index (tsm->list_pool, elt_index);
                      ses_index = elt->value;

                      if (!addr_only)
                        {
                          if ((s->out2in.addr.as_u32 != e_addr.as_u32) &&
                              (clib_net_to_host_u16 (s->out2in.port) != e_port))
                            continue;
                        }

                      /* log NAT event */
                      snat_ipfix_logging_nat44_ses_delete(s->in2out.addr.as_u32,
                                                          s->out2in.addr.as_u32,
                                                          s->in2out.protocol,
                                                          s->in2out.port,
                                                          s->out2in.port,
                                                          s->in2out.fib_index);

                      value.key = s->in2out.as_u64;
                      clib_bihash_add_del_8_8 (&sm->in2out, &value, 0);
                      value.key = s->out2in.as_u64;
                      clib_bihash_add_del_8_8 (&sm->out2in, &value, 0);
                      pool_put (tsm->sessions, s);

                      clib_dlist_remove (tsm->list_pool, del_elt_index);
                      pool_put_index (tsm->list_pool, del_elt_index);
                      u->nstaticsessions--;

                      if (!addr_only)
                        break;
                    }
                  if (addr_only)
                    {
                      pool_put (tsm->users, u);
                      clib_bihash_add_del_8_8 (&sm->user_hash, &kv, 0);
                    }
                }
            }
        }

      /* Delete static mapping from pool */
      pool_put (sm->static_mappings, m);
    }

  if (!addr_only)
    return 0;

  /* Add/delete external address to FIB */
  pool_foreach (interface, sm->interfaces,
  ({
    if (interface->is_inside)
      continue;

    snat_add_del_addr_to_fib(&e_addr, 32, interface->sw_if_index, is_add);
    break;
  }));

  return 0;
}

int snat_del_address (snat_main_t *sm, ip4_address_t addr, u8 delete_sm)
{
  snat_address_t *a = 0;
  snat_session_t *ses;
  u32 *ses_to_be_removed = 0, *ses_index;
  clib_bihash_kv_8_8_t kv, value;
  snat_user_key_t user_key;
  snat_user_t *u;
  snat_main_per_thread_data_t *tsm;
  snat_static_mapping_t *m;
  snat_interface_t *interface;
  int i;

  /* Find SNAT address */
  for (i=0; i < vec_len (sm->addresses); i++)
    {
      if (sm->addresses[i].addr.as_u32 == addr.as_u32)
        {
          a = sm->addresses + i;
          break;
        }
    }
  if (!a)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  if (delete_sm)
    {
      pool_foreach (m, sm->static_mappings,
      ({
          if (m->external_addr.as_u32 == addr.as_u32)
            (void) snat_add_static_mapping (m->local_addr, m->external_addr,
                                            m->local_port, m->external_port,
                                            m->vrf_id, m->addr_only, ~0,
                                            m->proto, 0);
      }));
    }
  else
    {
      /* Check if address is used in some static mapping */
      if (is_snat_address_used_in_static_mapping(sm, addr))
        {
          clib_warning ("address used in static mapping");
          return VNET_API_ERROR_UNSPECIFIED;
        }
    }

  /* Delete sessions using address */
  if (a->busy_tcp_ports || a->busy_udp_ports || a->busy_icmp_ports)
    {
      vec_foreach (tsm, sm->per_thread_data)
        {
          pool_foreach (ses, tsm->sessions, ({
            if (ses->out2in.addr.as_u32 == addr.as_u32)
              {
                /* log NAT event */
                snat_ipfix_logging_nat44_ses_delete(ses->in2out.addr.as_u32,
                                                    ses->out2in.addr.as_u32,
                                                    ses->in2out.protocol,
                                                    ses->in2out.port,
                                                    ses->out2in.port,
                                                    ses->in2out.fib_index);
                vec_add1 (ses_to_be_removed, ses - tsm->sessions);
                kv.key = ses->in2out.as_u64;
                clib_bihash_add_del_8_8 (&sm->in2out, &kv, 0);
                kv.key = ses->out2in.as_u64;
                clib_bihash_add_del_8_8 (&sm->out2in, &kv, 0);
                clib_dlist_remove (tsm->list_pool, ses->per_user_index);
                user_key.addr = ses->in2out.addr;
                user_key.fib_index = ses->in2out.fib_index;
                kv.key = user_key.as_u64;
                if (!clib_bihash_search_8_8 (&sm->user_hash, &kv, &value))
                  {
                    u = pool_elt_at_index (tsm->users, value.value);
                    u->nsessions--;
                  }
              }
          }));

          vec_foreach (ses_index, ses_to_be_removed)
            pool_put_index (tsm->sessions, ses_index[0]);

          vec_free (ses_to_be_removed);
       }
    }

  vec_del1 (sm->addresses, i);

  /* Delete external address from FIB */
  pool_foreach (interface, sm->interfaces,
  ({
    if (interface->is_inside)
      continue;

    snat_add_del_addr_to_fib(&addr, 32, interface->sw_if_index, 0);
    break;
  }));

  return 0;
}

int snat_interface_add_del (u32 sw_if_index, u8 is_inside, int is_del)
{
  snat_main_t *sm = &snat_main;
  snat_interface_t *i;
  const char * feature_name;
  snat_address_t * ap;
  snat_static_mapping_t * m;
  snat_det_map_t * dm;

  if (sm->static_mapping_only && !(sm->static_mapping_connection_tracking))
    feature_name = is_inside ?  "snat-in2out-fast" : "snat-out2in-fast";
  else
    {
      if (sm->num_workers > 1 && !sm->deterministic)
        feature_name = is_inside ?  "snat-in2out-worker-handoff" : "snat-out2in-worker-handoff";
      else if (sm->deterministic)
        feature_name = is_inside ?  "snat-det-in2out" : "snat-det-out2in";
      else
        feature_name = is_inside ?  "snat-in2out" : "snat-out2in";
    }

  vnet_feature_enable_disable ("ip4-unicast", feature_name, sw_if_index,
			       !is_del, 0, 0);

  if (sm->fq_in2out_index == ~0 && !sm->deterministic && sm->num_workers > 1)
    sm->fq_in2out_index = vlib_frame_queue_main_init (sm->in2out_node_index, 0);

  if (sm->fq_out2in_index == ~0 && !sm->deterministic && sm->num_workers > 1)
    sm->fq_out2in_index = vlib_frame_queue_main_init (sm->out2in_node_index, 0);

  pool_foreach (i, sm->interfaces,
  ({
    if (i->sw_if_index == sw_if_index)
      {
        if (is_del)
          pool_put (sm->interfaces, i);
        else
          return VNET_API_ERROR_VALUE_EXIST;

        goto fib;
      }
  }));

  if (is_del)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  pool_get (sm->interfaces, i);
  i->sw_if_index = sw_if_index;
  i->is_inside = is_inside;

  /* Add/delete external addresses to FIB */
fib:
  if (is_inside)
    return 0;

  vec_foreach (ap, sm->addresses)
    snat_add_del_addr_to_fib(&ap->addr, 32, sw_if_index, !is_del);

  pool_foreach (m, sm->static_mappings,
  ({
    if (!(m->addr_only))
      continue;

    snat_add_del_addr_to_fib(&m->external_addr, 32, sw_if_index, !is_del);
  }));

  pool_foreach (dm, sm->det_maps,
  ({
    snat_add_del_addr_to_fib(&dm->out_addr, dm->out_plen, sw_if_index, !is_del);
  }));

  return 0;
}

int snat_set_workers (uword * bitmap)
{
  snat_main_t *sm = &snat_main;
  int i;

  if (sm->num_workers < 2)
    return VNET_API_ERROR_FEATURE_DISABLED;

  if (clib_bitmap_last_set (bitmap) >= sm->num_workers)
    return VNET_API_ERROR_INVALID_WORKER;

  vec_free (sm->workers);
  clib_bitmap_foreach (i, bitmap,
    ({
      vec_add1(sm->workers, i);
    }));

  return 0;
}


static void
snat_ip4_add_del_interface_address_cb (ip4_main_t * im,
                                       uword opaque,
                                       u32 sw_if_index,
                                       ip4_address_t * address,
                                       u32 address_length,
                                       u32 if_address_index,
                                       u32 is_delete);

static clib_error_t * snat_init (vlib_main_t * vm)
{
  snat_main_t * sm = &snat_main;
  clib_error_t * error = 0;
  ip4_main_t * im = &ip4_main;
  ip_lookup_main_t * lm = &im->lookup_main;
  uword *p;
  vlib_thread_registration_t *tr;
  vlib_thread_main_t *tm = vlib_get_thread_main ();
  uword *bitmap = 0;
  u32 i;
  ip4_add_del_interface_address_callback_t cb4;

  sm->vlib_main = vm;
  sm->vnet_main = vnet_get_main();
  sm->ip4_main = im;
  sm->ip4_lookup_main = lm;
  sm->api_main = &api_main;
  sm->first_worker_index = 0;
  sm->next_worker = 0;
  sm->num_workers = 0;
  sm->workers = 0;
  sm->fq_in2out_index = ~0;
  sm->fq_out2in_index = ~0;
  sm->udp_timeout = SNAT_UDP_TIMEOUT;
  sm->tcp_established_timeout = SNAT_TCP_ESTABLISHED_TIMEOUT;
  sm->tcp_transitory_timeout = SNAT_TCP_TRANSITORY_TIMEOUT;
  sm->icmp_timeout = SNAT_ICMP_TIMEOUT;

  p = hash_get_mem (tm->thread_registrations_by_name, "workers");
  if (p)
    {
      tr = (vlib_thread_registration_t *) p[0];
      if (tr)
        {
          sm->num_workers = tr->count;
          sm->first_worker_index = tr->first_index;
        }
    }

  /* Use all available workers by default */
  if (sm->num_workers > 1)
    {
      for (i=0; i < sm->num_workers; i++)
        bitmap = clib_bitmap_set (bitmap, i, 1);
      snat_set_workers(bitmap);
      clib_bitmap_free (bitmap);
    }

  error = snat_api_init(vm, sm);
  if (error)
    return error;

  /* Set up the interface address add/del callback */
  cb4.function = snat_ip4_add_del_interface_address_cb;
  cb4.function_opaque = 0;

  vec_add1 (im->add_del_interface_address_callbacks, cb4);

  /* Init IPFIX logging */
  snat_ipfix_logging_init(vm);

  error = nat64_init(vm);

  return error;
}

VLIB_INIT_FUNCTION (snat_init);

void snat_free_outside_address_and_port (snat_main_t * sm, 
                                         snat_session_key_t * k, 
                                         u32 address_index)
{
  snat_address_t *a;
  u16 port_host_byte_order = clib_net_to_host_u16 (k->port);
  
  ASSERT (address_index < vec_len (sm->addresses));

  a = sm->addresses + address_index;

  switch (k->protocol)
    {
#define _(N, i, n, s) \
    case SNAT_PROTOCOL_##N: \
      ASSERT (clib_bitmap_get_no_check (a->busy_##n##_port_bitmap, \
        port_host_byte_order) == 1); \
      clib_bitmap_set_no_check (a->busy_##n##_port_bitmap, \
        port_host_byte_order, 0); \
      a->busy_##n##_ports--; \
      break;
      foreach_snat_protocol
#undef _
    default:
      clib_warning("unknown_protocol");
      return;
    }
}  

/**
 * @brief Match SNAT static mapping.
 *
 * @param sm          SNAT main.
 * @param match       Address and port to match.
 * @param mapping     External or local address and port of the matched mapping.
 * @param by_external If 0 match by local address otherwise match by external
 *                    address.
 * @param is_addr_only If matched mapping is address only
 *
 * @returns 0 if match found otherwise 1.
 */
int snat_static_mapping_match (snat_main_t * sm,
                               snat_session_key_t match,
                               snat_session_key_t * mapping,
                               u8 by_external,
                               u8 *is_addr_only)
{
  clib_bihash_kv_8_8_t kv, value;
  snat_static_mapping_t *m;
  snat_session_key_t m_key;
  clib_bihash_8_8_t *mapping_hash = &sm->static_mapping_by_local;

  if (by_external)
    mapping_hash = &sm->static_mapping_by_external;

  m_key.addr = match.addr;
  m_key.port = clib_net_to_host_u16 (match.port);
  m_key.protocol = match.protocol;
  m_key.fib_index = match.fib_index;

  kv.key = m_key.as_u64;

  if (clib_bihash_search_8_8 (mapping_hash, &kv, &value))
    {
      /* Try address only mapping */
      m_key.port = 0;
      m_key.protocol = 0;
      kv.key = m_key.as_u64;
      if (clib_bihash_search_8_8 (mapping_hash, &kv, &value))
        return 1;
    }

  m = pool_elt_at_index (sm->static_mappings, value.value);

  if (by_external)
    {
      mapping->addr = m->local_addr;
      /* Address only mapping doesn't change port */
      mapping->port = m->addr_only ? match.port
        : clib_host_to_net_u16 (m->local_port);
      mapping->fib_index = m->fib_index;
    }
  else
    {
      mapping->addr = m->external_addr;
      /* Address only mapping doesn't change port */
      mapping->port = m->addr_only ? match.port
        : clib_host_to_net_u16 (m->external_port);
      mapping->fib_index = sm->outside_fib_index;
    }

  if (PREDICT_FALSE(is_addr_only != 0))
    *is_addr_only = m->addr_only;

  return 0;
}

int snat_alloc_outside_address_and_port (snat_main_t * sm, 
                                         u32 fib_index,
                                         snat_session_key_t * k,
                                         u32 * address_indexp)
{
  int i;
  snat_address_t *a;
  u32 portnum;

  for (i = 0; i < vec_len (sm->addresses); i++)
    {
      a = sm->addresses + i;
      if (sm->vrf_mode && a->fib_index != ~0 && a->fib_index != fib_index)
        continue;
      switch (k->protocol)
        {
#define _(N, j, n, s) \
        case SNAT_PROTOCOL_##N: \
          if (a->busy_##n##_ports < (65535-1024)) \
            { \
              while (1) \
                { \
                  portnum = random_u32 (&sm->random_seed); \
                  portnum &= 0xFFFF; \
                  if (portnum < 1024) \
                    continue; \
                  if (clib_bitmap_get_no_check (a->busy_##n##_port_bitmap, portnum)) \
                    continue; \
                  clib_bitmap_set_no_check (a->busy_##n##_port_bitmap, portnum, 1); \
                  a->busy_##n##_ports++; \
                  k->addr = a->addr; \
                  k->port = clib_host_to_net_u16(portnum); \
                  *address_indexp = i; \
                  return 0; \
                } \
            } \
          break;
          foreach_snat_protocol
#undef _
        default:
          clib_warning("unknown protocol");
          return 1;
        }

    }
  /* Totally out of translations to use... */
  snat_ipfix_logging_addresses_exhausted(0);
  return 1;
}


static clib_error_t *
add_address_command_fn (vlib_main_t * vm,
                        unformat_input_t * input,
                        vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  snat_main_t * sm = &snat_main;
  ip4_address_t start_addr, end_addr, this_addr;
  u32 start_host_order, end_host_order;
  u32 vrf_id = ~0;
  int i, count;
  int is_add = 1;
  int rv = 0;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U - %U",
                    unformat_ip4_address, &start_addr,
                    unformat_ip4_address, &end_addr))
        ;
      else if (unformat (line_input, "tenant-vrf %u", &vrf_id))
        ;
      else if (unformat (line_input, "%U", unformat_ip4_address, &start_addr))
        end_addr = start_addr;
      else if (unformat (line_input, "del"))
        is_add = 0;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
            format_unformat_error, line_input);
          goto done;
        }
     }

  if (sm->static_mapping_only)
    {
      error = clib_error_return (0, "static mapping only mode");
      goto done;
    }

  start_host_order = clib_host_to_net_u32 (start_addr.as_u32);
  end_host_order = clib_host_to_net_u32 (end_addr.as_u32);
  
  if (end_host_order < start_host_order)
    {
      error = clib_error_return (0, "end address less than start address");
      goto done;
    }

  count = (end_host_order - start_host_order) + 1;

  if (count > 1024)
    clib_warning ("%U - %U, %d addresses...",
                  format_ip4_address, &start_addr,
                  format_ip4_address, &end_addr,
                  count);
  
  this_addr = start_addr;

  for (i = 0; i < count; i++)
    {
      if (is_add)
        snat_add_address (sm, &this_addr, vrf_id);
      else
        rv = snat_del_address (sm, this_addr, 0);

      switch (rv)
        {
        case VNET_API_ERROR_NO_SUCH_ENTRY:
          error = clib_error_return (0, "S-NAT address not exist.");
          goto done;
        case VNET_API_ERROR_UNSPECIFIED:
          error = clib_error_return (0, "S-NAT address used in static mapping.");
          goto done;
        default:
          break;
        }

      increment_v4_address (&this_addr);
    }

done:
  unformat_free (line_input);

  return error;
}

VLIB_CLI_COMMAND (add_address_command, static) = {
  .path = "snat add address",
  .short_help = "snat add addresses <ip4-range-start> [- <ip4-range-end>] "
                "[tenant-vrf <vrf-id>] [del]",
  .function = add_address_command_fn,
};

static clib_error_t *
snat_feature_command_fn (vlib_main_t * vm,
                          unformat_input_t * input,
                          vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  vnet_main_t * vnm = vnet_get_main();
  clib_error_t * error = 0;
  u32 sw_if_index;
  u32 * inside_sw_if_indices = 0;
  u32 * outside_sw_if_indices = 0;
  int is_del = 0;
  int i;

  sw_if_index = ~0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "in %U", unformat_vnet_sw_interface,
                    vnm, &sw_if_index))
        vec_add1 (inside_sw_if_indices, sw_if_index);
      else if (unformat (line_input, "out %U", unformat_vnet_sw_interface,
                         vnm, &sw_if_index))
        vec_add1 (outside_sw_if_indices, sw_if_index);
      else if (unformat (line_input, "del"))
        is_del = 1;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
            format_unformat_error, line_input);
          goto done;
        }
    }

  if (vec_len (inside_sw_if_indices))
    {
      for (i = 0; i < vec_len(inside_sw_if_indices); i++)
        {
          sw_if_index = inside_sw_if_indices[i];
          snat_interface_add_del (sw_if_index, 1, is_del);
        }
    }

  if (vec_len (outside_sw_if_indices))
    {
      for (i = 0; i < vec_len(outside_sw_if_indices); i++)
        {
          sw_if_index = outside_sw_if_indices[i];
          snat_interface_add_del (sw_if_index, 0, is_del);
        }
    }

done:
  unformat_free (line_input);
  vec_free (inside_sw_if_indices);
  vec_free (outside_sw_if_indices);

  return error;
}

VLIB_CLI_COMMAND (set_interface_snat_command, static) = {
  .path = "set interface snat",
  .function = snat_feature_command_fn,
  .short_help = "set interface snat in <intfc> out <intfc> [del]",
};

uword
unformat_snat_protocol (unformat_input_t * input, va_list * args)
{
  u32 *r = va_arg (*args, u32 *);

  if (0);
#define _(N, i, n, s) else if (unformat (input, s)) *r = SNAT_PROTOCOL_##N;
  foreach_snat_protocol
#undef _
  else
    return 0;
  return 1;
}

u8 *
format_snat_protocol (u8 * s, va_list * args)
{
  u32 i = va_arg (*args, u32);
  u8 *t = 0;

  switch (i)
    {
#define _(N, j, n, str) case SNAT_PROTOCOL_##N: t = (u8 *) str; break;
      foreach_snat_protocol
#undef _
    default:
      s = format (s, "unknown");
    }
  s = format (s, "%s", t);
  return s;
}

static clib_error_t *
add_static_mapping_command_fn (vlib_main_t * vm,
                               unformat_input_t * input,
                               vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t * error = 0;
  ip4_address_t l_addr, e_addr;
  u32 l_port = 0, e_port = 0, vrf_id = ~0;
  int is_add = 1;
  int addr_only = 1;
  u32 sw_if_index = ~0;
  vnet_main_t * vnm = vnet_get_main();
  int rv;
  snat_protocol_t proto;
  u8 proto_set = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "local %U %u", unformat_ip4_address, &l_addr,
                    &l_port))
        addr_only = 0;
      else if (unformat (line_input, "local %U", unformat_ip4_address, &l_addr))
        ;
      else if (unformat (line_input, "external %U %u", unformat_ip4_address,
                         &e_addr, &e_port))
        addr_only = 0;
      else if (unformat (line_input, "external %U", unformat_ip4_address,
                         &e_addr))
        ;
      else if (unformat (line_input, "external %U %u",
                         unformat_vnet_sw_interface, vnm, &sw_if_index,
                         &e_port))
        addr_only = 0;

      else if (unformat (line_input, "external %U",
                         unformat_vnet_sw_interface, vnm, &sw_if_index))
        ;
      else if (unformat (line_input, "vrf %u", &vrf_id))
        ;
      else if (unformat (line_input, "%U", unformat_snat_protocol, &proto))
        proto_set = 1;
      else if (unformat (line_input, "del"))
        is_add = 0;
      else
        {
          error = clib_error_return (0, "unknown input: '%U'",
            format_unformat_error, line_input);
          goto done;
        }
    }

  if (!addr_only && !proto_set)
    {
      error = clib_error_return (0, "missing protocol");
      goto done;
    }

  rv = snat_add_static_mapping(l_addr, e_addr, (u16) l_port, (u16) e_port,
                               vrf_id, addr_only, sw_if_index, proto, is_add);

  switch (rv)
    {
    case VNET_API_ERROR_INVALID_VALUE:
      error = clib_error_return (0, "External port already in use.");
      goto done;
    case VNET_API_ERROR_NO_SUCH_ENTRY:
      if (is_add)
        error = clib_error_return (0, "External addres must be allocated.");
      else
        error = clib_error_return (0, "Mapping not exist.");
      goto done;
    case VNET_API_ERROR_NO_SUCH_FIB:
      error = clib_error_return (0, "No such VRF id.");
      goto done;
    case VNET_API_ERROR_VALUE_EXIST:
      error = clib_error_return (0, "Mapping already exist.");
      goto done;
    default:
      break;
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat add static mapping}
 * Static mapping allows hosts on the external network to initiate connection
 * to to the local network host.
 * To create static mapping between local host address 10.0.0.3 port 6303 and
 * external address 4.4.4.4 port 3606 for TCP protocol use:
 *  vpp# snat add static mapping local tcp 10.0.0.3 6303 external 4.4.4.4 3606
 * If not runnig "static mapping only" S-NAT plugin mode use before:
 *  vpp# snat add address 4.4.4.4
 * To create static mapping between local and external address use:
 *  vpp# snat add static mapping local 10.0.0.3 external 4.4.4.4
 * @cliexend
?*/
VLIB_CLI_COMMAND (add_static_mapping_command, static) = {
  .path = "snat add static mapping",
  .function = add_static_mapping_command_fn,
  .short_help =
    "snat add static mapping local tcp|udp|icmp <addr> [<port>] external <addr> [<port>] [vrf <table-id>] [del]",
};

static clib_error_t *
set_workers_command_fn (vlib_main_t * vm,
                        unformat_input_t * input,
                        vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  uword *bitmap = 0;
  int rv = 0;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U", unformat_bitmap_list, &bitmap))
        ;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
            format_unformat_error, line_input);
          goto done;
        }
     }

  if (bitmap == 0)
    {
      error = clib_error_return (0, "List of workers must be specified.");
      goto done;
    }

  rv = snat_set_workers(bitmap);

  clib_bitmap_free (bitmap);

  switch (rv)
    {
    case VNET_API_ERROR_INVALID_WORKER:
      error = clib_error_return (0, "Invalid worker(s).");
      goto done;
    case VNET_API_ERROR_FEATURE_DISABLED:
      error = clib_error_return (0,
        "Supported only if 2 or more workes available.");
      goto done;
    default:
      break;
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{set snat workers}
 * Set SNAT workers if 2 or more workers available, use:
 *  vpp# set snat workers 0-2,5
 * @cliexend
?*/
VLIB_CLI_COMMAND (set_workers_command, static) = {
  .path = "set snat workers",
  .function = set_workers_command_fn,
  .short_help =
    "set snat workers <workers-list>",
};

static clib_error_t *
snat_ipfix_logging_enable_disable_command_fn (vlib_main_t * vm,
                                              unformat_input_t * input,
                                              vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  u32 domain_id = 0;
  u32 src_port = 0;
  u8 enable = 1;
  int rv = 0;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "domain %d", &domain_id))
        ;
      else if (unformat (line_input, "src-port %d", &src_port))
        ;
      else if (unformat (line_input, "disable"))
        enable = 0;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
            format_unformat_error, line_input);
          goto done;
        }
     }

  rv = snat_ipfix_logging_enable_disable (enable, domain_id, (u16) src_port);

  if (rv)
    {
      error = clib_error_return (0, "ipfix logging enable failed");
      goto done;
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat ipfix logging}
 * To enable SNAT IPFIX logging use:
 *  vpp# snat ipfix logging
 * To set IPFIX exporter use:
 *  vpp# set ipfix exporter collector 10.10.10.3 src 10.10.10.1
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_ipfix_logging_enable_disable_command, static) = {
  .path = "snat ipfix logging",
  .function = snat_ipfix_logging_enable_disable_command_fn,
  .short_help = "snat ipfix logging [domain <domain-id>] [src-port <port>] [disable]",
};

static u32
snat_get_worker_in2out_cb (ip4_header_t * ip0, u32 rx_fib_index0)
{
  snat_main_t *sm = &snat_main;
  snat_user_key_t key0;
  clib_bihash_kv_8_8_t kv0, value0;
  u32 next_worker_index = 0;

  key0.addr = ip0->src_address;
  key0.fib_index = rx_fib_index0;

  kv0.key = key0.as_u64;

  /* Ever heard of of the "user" before? */
  if (clib_bihash_search_8_8 (&sm->worker_by_in, &kv0, &value0))
    {
      /* No, assign next available worker (RR) */
      next_worker_index = sm->first_worker_index;
      if (vec_len (sm->workers))
        {
          next_worker_index +=
            sm->workers[sm->next_worker++ % _vec_len (sm->workers)];
        }

      /* add non-traslated packets worker lookup */
      kv0.value = next_worker_index;
      clib_bihash_add_del_8_8 (&sm->worker_by_in, &kv0, 1);
    }
  else
    next_worker_index = value0.value;

  return next_worker_index;
}

static u32
snat_get_worker_out2in_cb (ip4_header_t * ip0, u32 rx_fib_index0)
{
  snat_main_t *sm = &snat_main;
  snat_worker_key_t key0;
  clib_bihash_kv_8_8_t kv0, value0;
  udp_header_t * udp0;
  u32 next_worker_index = 0;

  udp0 = ip4_next_header (ip0);

  key0.addr = ip0->dst_address;
  key0.port = udp0->dst_port;
  key0.fib_index = rx_fib_index0;

  if (PREDICT_FALSE(ip0->protocol == IP_PROTOCOL_ICMP))
    {
      icmp46_header_t * icmp0 = (icmp46_header_t *) udp0;
      icmp_echo_header_t *echo0 = (icmp_echo_header_t *)(icmp0+1);
      key0.port = echo0->identifier;
    }

  kv0.key = key0.as_u64;

  /* Ever heard of of the "user" before? */
  if (clib_bihash_search_8_8 (&sm->worker_by_out, &kv0, &value0))
    {
      key0.port = 0;
      kv0.key = key0.as_u64;

      if (clib_bihash_search_8_8 (&sm->worker_by_out, &kv0, &value0))
        {
          /* No, assign next available worker (RR) */
          next_worker_index = sm->first_worker_index;
          if (vec_len (sm->workers))
            {
              next_worker_index +=
                sm->workers[sm->next_worker++ % _vec_len (sm->workers)];
            }
        }
      else
        {
          /* Static mapping without port */
          next_worker_index = value0.value;
        }

      /* Add to translated packets worker lookup */
      kv0.value = next_worker_index;
      clib_bihash_add_del_8_8 (&sm->worker_by_out, &kv0, 1);
    }
  else
    next_worker_index = value0.value;

  return next_worker_index;
}

static clib_error_t *
snat_config (vlib_main_t * vm, unformat_input_t * input)
{
  snat_main_t * sm = &snat_main;
  u32 translation_buckets = 1024;
  u32 translation_memory_size = 128<<20;
  u32 user_buckets = 128;
  u32 user_memory_size = 64<<20;
  u32 max_translations_per_user = 100;
  u32 outside_vrf_id = 0;
  u32 inside_vrf_id = 0;
  u32 static_mapping_buckets = 1024;
  u32 static_mapping_memory_size = 64<<20;
  u8 static_mapping_only = 0;
  u8 static_mapping_connection_tracking = 0;
  vlib_thread_main_t *tm = vlib_get_thread_main ();

  sm->deterministic = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "translation hash buckets %d", &translation_buckets))
        ;
      else if (unformat (input, "translation hash memory %d",
                         &translation_memory_size));
      else if (unformat (input, "user hash buckets %d", &user_buckets))
        ;
      else if (unformat (input, "user hash memory %d",
                         &user_memory_size))
        ;
      else if (unformat (input, "max translations per user %d",
                         &max_translations_per_user))
        ;
      else if (unformat (input, "outside VRF id %d",
                         &outside_vrf_id))
        ;
      else if (unformat (input, "inside VRF id %d",
                         &inside_vrf_id))
        ;
      else if (unformat (input, "static mapping only"))
        {
          static_mapping_only = 1;
          if (unformat (input, "connection tracking"))
            static_mapping_connection_tracking = 1;
        }
      else if (unformat (input, "deterministic"))
        sm->deterministic = 1;
      else
	return clib_error_return (0, "unknown input '%U'",
				  format_unformat_error, input);
    }

  /* for show commands, etc. */
  sm->translation_buckets = translation_buckets;
  sm->translation_memory_size = translation_memory_size;
  sm->user_buckets = user_buckets;
  sm->user_memory_size = user_memory_size;
  sm->max_translations_per_user = max_translations_per_user;
  sm->outside_vrf_id = outside_vrf_id;
  sm->outside_fib_index = fib_table_find_or_create_and_lock (FIB_PROTOCOL_IP4,
                                                             outside_vrf_id);
  sm->inside_vrf_id = inside_vrf_id;
  sm->inside_fib_index = fib_table_find_or_create_and_lock (FIB_PROTOCOL_IP4,
                                                            inside_vrf_id);
  sm->static_mapping_only = static_mapping_only;
  sm->static_mapping_connection_tracking = static_mapping_connection_tracking;

  if (sm->deterministic)
    {
      sm->in2out_node_index = snat_det_in2out_node.index;
      sm->out2in_node_index = snat_det_out2in_node.index;
      sm->icmp_match_in2out_cb = icmp_match_in2out_det;
      sm->icmp_match_out2in_cb = icmp_match_out2in_det;
    }
  else
    {
      sm->worker_in2out_cb = snat_get_worker_in2out_cb;
      sm->worker_out2in_cb = snat_get_worker_out2in_cb;
      sm->in2out_node_index = snat_in2out_node.index;
      sm->out2in_node_index = snat_out2in_node.index;
      if (!static_mapping_only ||
          (static_mapping_only && static_mapping_connection_tracking))
        {
          sm->icmp_match_in2out_cb = icmp_match_in2out_slow;
          sm->icmp_match_out2in_cb = icmp_match_out2in_slow;

          clib_bihash_init_8_8 (&sm->worker_by_in, "worker-by-in", user_buckets,
                                user_memory_size);

          clib_bihash_init_8_8 (&sm->worker_by_out, "worker-by-out", user_buckets,
                                user_memory_size);

          vec_validate (sm->per_thread_data, tm->n_vlib_mains - 1);

          clib_bihash_init_8_8 (&sm->in2out, "in2out", translation_buckets,
                                translation_memory_size);

          clib_bihash_init_8_8 (&sm->out2in, "out2in", translation_buckets,
                                translation_memory_size);

          clib_bihash_init_8_8 (&sm->user_hash, "users", user_buckets,
                                user_memory_size);
        }
      else
        {
          sm->icmp_match_in2out_cb = icmp_match_in2out_fast;
          sm->icmp_match_out2in_cb = icmp_match_out2in_fast;
        }
      clib_bihash_init_8_8 (&sm->static_mapping_by_local,
                            "static_mapping_by_local", static_mapping_buckets,
                            static_mapping_memory_size);

      clib_bihash_init_8_8 (&sm->static_mapping_by_external,
                            "static_mapping_by_external", static_mapping_buckets,
                            static_mapping_memory_size);
    }

  return 0;
}

VLIB_CONFIG_FUNCTION (snat_config, "snat");

u8 * format_snat_session_state (u8 * s, va_list * args)
{
  u32 i = va_arg (*args, u32);
  u8 *t = 0;

  switch (i)
    {
#define _(v, N, str) case SNAT_SESSION_##N: t = (u8 *) str; break;
    foreach_snat_session_state
#undef _
    default:
      t = format (t, "unknown");
    }
  s = format (s, "%s", t);
  return s;
}

u8 * format_snat_key (u8 * s, va_list * args)
{
  snat_session_key_t * key = va_arg (*args, snat_session_key_t *);
  char * protocol_string = "unknown";
  static char *protocol_strings[] = {
      "UDP",
      "TCP",
      "ICMP",
  };

  if (key->protocol < ARRAY_LEN(protocol_strings))
      protocol_string = protocol_strings[key->protocol];

  s = format (s, "%U proto %s port %d fib %d",
              format_ip4_address, &key->addr, protocol_string,
              clib_net_to_host_u16 (key->port), key->fib_index);
  return s;
}

u8 * format_snat_session (u8 * s, va_list * args)
{
  snat_main_t * sm __attribute__((unused)) = va_arg (*args, snat_main_t *);
  snat_session_t * sess = va_arg (*args, snat_session_t *);

  s = format (s, "  i2o %U\n", format_snat_key, &sess->in2out);
  s = format (s, "    o2i %U\n", format_snat_key, &sess->out2in);
  s = format (s, "       last heard %.2f\n", sess->last_heard);
  s = format (s, "       total pkts %d, total bytes %lld\n",
              sess->total_pkts, sess->total_bytes);
  if (snat_is_session_static (sess))
    s = format (s, "       static translation\n");
  else
    s = format (s, "       dynamic translation\n");

  return s;
}

u8 * format_snat_user (u8 * s, va_list * args)
{
  snat_main_per_thread_data_t * sm = va_arg (*args, snat_main_per_thread_data_t *);
  snat_user_t * u = va_arg (*args, snat_user_t *);
  int verbose = va_arg (*args, int);
  dlist_elt_t * head, * elt;
  u32 elt_index, head_index;
  u32 session_index;
  snat_session_t * sess;

  s = format (s, "%U: %d dynamic translations, %d static translations\n",
              format_ip4_address, &u->addr, u->nsessions, u->nstaticsessions);

  if (verbose == 0)
    return s;

  if (u->nsessions || u->nstaticsessions)
    {
      head_index = u->sessions_per_user_list_head_index;
      head = pool_elt_at_index (sm->list_pool, head_index);

      elt_index = head->next;
      elt = pool_elt_at_index (sm->list_pool, elt_index);
      session_index = elt->value;

      while (session_index != ~0)
        {
          sess = pool_elt_at_index (sm->sessions, session_index);

          s = format (s, "  %U\n", format_snat_session, sm, sess);

          elt_index = elt->next;
          elt = pool_elt_at_index (sm->list_pool, elt_index);
          session_index = elt->value;
        }
    }

  return s;
}

u8 * format_snat_static_mapping (u8 * s, va_list * args)
{
  snat_static_mapping_t *m = va_arg (*args, snat_static_mapping_t *);

  if (m->addr_only)
      s = format (s, "local %U external %U vrf %d",
                  format_ip4_address, &m->local_addr,
                  format_ip4_address, &m->external_addr,
                  m->vrf_id);
  else
      s = format (s, "%U local %U:%d external %U:%d vrf %d",
                  format_snat_protocol, m->proto,
                  format_ip4_address, &m->local_addr, m->local_port,
                  format_ip4_address, &m->external_addr, m->external_port,
                  m->vrf_id);

  return s;
}

u8 * format_snat_static_map_to_resolve (u8 * s, va_list * args)
{
  snat_static_map_resolve_t *m = va_arg (*args, snat_static_map_resolve_t *);
  vnet_main_t *vnm = vnet_get_main();

  if (m->addr_only)
      s = format (s, "local %U external %U vrf %d",
                  format_ip4_address, &m->l_addr,
                  format_vnet_sw_interface_name, vnm,
                  vnet_get_sw_interface (vnm, m->sw_if_index),
                  m->vrf_id);
  else
      s = format (s, "%U local %U:%d external %U:%d vrf %d",
                  format_snat_protocol, m->proto,
                  format_ip4_address, &m->l_addr, m->l_port,
                  format_vnet_sw_interface_name, vnm,
                  vnet_get_sw_interface (vnm, m->sw_if_index), m->e_port,
                  m->vrf_id);

  return s;
}

u8 * format_det_map_ses (u8 * s, va_list * args)
{
  snat_det_map_t * det_map = va_arg (*args, snat_det_map_t *);
  ip4_address_t in_addr, out_addr;
  u32 in_offset, out_offset;
  snat_det_session_t * ses = va_arg (*args, snat_det_session_t *);
  u32 * i = va_arg (*args, u32 *);

  u32 user_index = *i / SNAT_DET_SES_PER_USER;
  in_addr.as_u32 = clib_host_to_net_u32 (
    clib_net_to_host_u32(det_map->in_addr.as_u32) + user_index);
  in_offset = clib_net_to_host_u32(in_addr.as_u32) -
    clib_net_to_host_u32(det_map->in_addr.as_u32);
  out_offset = in_offset / det_map->sharing_ratio;
  out_addr.as_u32 = clib_host_to_net_u32(
    clib_net_to_host_u32(det_map->out_addr.as_u32) + out_offset);
  s = format (s, "in %U:%d out %U:%d external host %U:%d state: %U expire: %d\n",
              format_ip4_address, &in_addr,
              clib_net_to_host_u16 (ses->in_port),
              format_ip4_address, &out_addr,
              clib_net_to_host_u16 (ses->out.out_port),
              format_ip4_address, &ses->out.ext_host_addr,
              clib_net_to_host_u16 (ses->out.ext_host_port),
              format_snat_session_state, ses->state,
              ses->expire);

  return s;
}

static clib_error_t *
show_snat_command_fn (vlib_main_t * vm,
		 unformat_input_t * input,
		 vlib_cli_command_t * cmd)
{
  int verbose = 0;
  snat_main_t * sm = &snat_main;
  snat_user_t * u;
  snat_static_mapping_t *m;
  snat_interface_t *i;
  snat_address_t * ap;
  vnet_main_t *vnm = vnet_get_main();
  snat_main_per_thread_data_t *tsm;
  u32 users_num = 0, sessions_num = 0, *worker, *sw_if_index;
  uword j = 0;
  snat_static_map_resolve_t *rp;
  snat_det_map_t * dm;
  snat_det_session_t * ses;

  if (unformat (input, "detail"))
    verbose = 1;
  else if (unformat (input, "verbose"))
    verbose = 2;

  if (sm->static_mapping_only)
    {
      if (sm->static_mapping_connection_tracking)
        vlib_cli_output (vm, "SNAT mode: static mapping only connection "
                         "tracking");
      else
        vlib_cli_output (vm, "SNAT mode: static mapping only");
    }
  else if (sm->deterministic)
    {
      vlib_cli_output (vm, "SNAT mode: deterministic mapping");
    }
  else
    {
      vlib_cli_output (vm, "SNAT mode: dynamic translations enabled");
    }

  if (verbose > 0)
    {
      pool_foreach (i, sm->interfaces,
      ({
        vlib_cli_output (vm, "%U %s", format_vnet_sw_interface_name, vnm,
                         vnet_get_sw_interface (vnm, i->sw_if_index),
                         i->is_inside ? "in" : "out");
      }));

      if (vec_len (sm->auto_add_sw_if_indices))
        {
          vlib_cli_output (vm, "SNAT pool addresses interfaces:");
          vec_foreach (sw_if_index, sm->auto_add_sw_if_indices)
            {
              vlib_cli_output (vm, "%U", format_vnet_sw_interface_name, vnm,
                               vnet_get_sw_interface (vnm, *sw_if_index));
            }
        }

      vec_foreach (ap, sm->addresses)
        {
          vlib_cli_output (vm, "%U", format_ip4_address, &ap->addr);
          if (ap->fib_index != ~0)
              vlib_cli_output (vm, "  tenant VRF: %u",
                               ip4_fib_get(ap->fib_index)->table_id);
          else
            vlib_cli_output (vm, "  tenant VRF independent");
#define _(N, i, n, s) \
          vlib_cli_output (vm, "  %d busy %s ports", ap->busy_##n##_ports, s);
          foreach_snat_protocol
#undef _
        }
    }

  if (sm->num_workers > 1)
    {
      vlib_cli_output (vm, "%d workers", vec_len (sm->workers));
      if (verbose > 0)
        {
          vec_foreach (worker, sm->workers)
            {
              vlib_worker_thread_t *w =
                vlib_worker_threads + *worker + sm->first_worker_index;
              vlib_cli_output (vm, "  %s", w->name);
            }
        }
    }

  if (sm->deterministic)
    {
      vlib_cli_output (vm, "udp timeout: %dsec", sm->udp_timeout);
      vlib_cli_output (vm, "tcp-established timeout: %dsec",
                       sm->tcp_established_timeout);
      vlib_cli_output (vm, "tcp-transitory timeout: %dsec",
                       sm->tcp_transitory_timeout);
      vlib_cli_output (vm, "icmp timeout: %dsec", sm->icmp_timeout);
      vlib_cli_output (vm, "%d deterministic mappings",
                       pool_elts (sm->det_maps));
      if (verbose > 0)
        {
          pool_foreach (dm, sm->det_maps,
          ({
            vlib_cli_output (vm, "in %U/%d out %U/%d\n",
                             format_ip4_address, &dm->in_addr, dm->in_plen,
                             format_ip4_address, &dm->out_addr, dm->out_plen);
            vlib_cli_output (vm, " outside address sharing ratio: %d\n",
                             dm->sharing_ratio);
            vlib_cli_output (vm, " number of ports per inside host: %d\n",
                             dm->ports_per_host);
            vlib_cli_output (vm, " sessions number: %d\n", dm->ses_num);
            if (verbose > 1)
              {
                vec_foreach_index (j, dm->sessions)
                  {
                    ses = vec_elt_at_index (dm->sessions, j);
                    if (ses->in_port)
                      vlib_cli_output (vm, "  %U", format_det_map_ses, dm, ses,
                                       &j);
                  }
              }
          }));
        }
    }
  else
    {
      if (sm->static_mapping_only && !(sm->static_mapping_connection_tracking))
        {
          vlib_cli_output (vm, "%d static mappings",
                           pool_elts (sm->static_mappings));

          if (verbose > 0)
            {
              pool_foreach (m, sm->static_mappings,
              ({
                vlib_cli_output (vm, "%U", format_snat_static_mapping, m);
              }));
            }
        }
      else
        {
          vec_foreach (tsm, sm->per_thread_data)
            {
              users_num += pool_elts (tsm->users);
              sessions_num += pool_elts (tsm->sessions);
            }

          vlib_cli_output (vm, "%d users, %d outside addresses, %d active sessions,"
                           " %d static mappings",
                           users_num,
                           vec_len (sm->addresses),
                           sessions_num,
                           pool_elts (sm->static_mappings));

          if (verbose > 0)
            {
              vlib_cli_output (vm, "%U", format_bihash_8_8, &sm->in2out,
                               verbose - 1);
              vlib_cli_output (vm, "%U", format_bihash_8_8, &sm->out2in,
                               verbose - 1);
              vlib_cli_output (vm, "%U", format_bihash_8_8, &sm->worker_by_in,
                               verbose - 1);
              vlib_cli_output (vm, "%U", format_bihash_8_8, &sm->worker_by_out,
                               verbose - 1);
              vec_foreach_index (j, sm->per_thread_data)
                {
                  tsm = vec_elt_at_index (sm->per_thread_data, j);

                  if (pool_elts (tsm->users) == 0)
                    continue;

                  vlib_worker_thread_t *w = vlib_worker_threads + j;
                  vlib_cli_output (vm, "Thread %d (%s at lcore %u):", j, w->name,
                                   w->lcore_id);
                  vlib_cli_output (vm, "  %d list pool elements",
                                   pool_elts (tsm->list_pool));

                  pool_foreach (u, tsm->users,
                  ({
                    vlib_cli_output (vm, "  %U", format_snat_user, tsm, u,
                                     verbose - 1);
                  }));
                }

              if (pool_elts (sm->static_mappings))
                {
                  vlib_cli_output (vm, "static mappings:");
                  pool_foreach (m, sm->static_mappings,
                  ({
                    vlib_cli_output (vm, "%U", format_snat_static_mapping, m);
                  }));
                  for (j = 0; j < vec_len (sm->to_resolve); j++)
                    {
                      rp = sm->to_resolve + j;
                      vlib_cli_output (vm, "%U",
                                       format_snat_static_map_to_resolve, rp);
                    }
                }
            }
        }
    }
  return 0;
}

VLIB_CLI_COMMAND (show_snat_command, static) = {
    .path = "show snat",
    .short_help = "show snat",
    .function = show_snat_command_fn,
};


static void
snat_ip4_add_del_interface_address_cb (ip4_main_t * im,
                                       uword opaque,
                                       u32 sw_if_index,
                                       ip4_address_t * address,
                                       u32 address_length,
                                       u32 if_address_index,
                                       u32 is_delete)
{
  snat_main_t *sm = &snat_main;
  snat_static_map_resolve_t *rp;
  u32 *indices_to_delete = 0;
  int i, j;
  int rv;

  for (i = 0; i < vec_len(sm->auto_add_sw_if_indices); i++)
    {
      if (sw_if_index == sm->auto_add_sw_if_indices[i])
        {
          if (!is_delete)
            {
              /* Don't trip over lease renewal, static config */
              for (j = 0; j < vec_len(sm->addresses); j++)
                if (sm->addresses[j].addr.as_u32 == address->as_u32)
                  return;

              snat_add_address (sm, address, ~0);
              /* Scan static map resolution vector */
              for (j = 0; j < vec_len (sm->to_resolve); j++)
                {
                  rp = sm->to_resolve + j;
                  /* On this interface? */
                  if (rp->sw_if_index == sw_if_index)
                    {
                      /* Add the static mapping */
                      rv = snat_add_static_mapping (rp->l_addr,
                                                    address[0],
                                                    rp->l_port,
                                                    rp->e_port,
                                                    rp->vrf_id,
                                                    rp->addr_only,
                                                    ~0 /* sw_if_index */,
                                                    rp->proto,
                                                    rp->is_add);
                      if (rv)
                        clib_warning ("snat_add_static_mapping returned %d", 
                                      rv);
                      vec_add1 (indices_to_delete, j);
                    }
                }
              /* If we resolved any of the outstanding static mappings */
              if (vec_len(indices_to_delete))
                {
                  /* Delete them */
                  for (j = vec_len(indices_to_delete)-1; j >= 0; j--)
                    vec_delete(sm->to_resolve, 1, j);
                  vec_free(indices_to_delete);
                }
              return;
            }
          else
            {
              (void) snat_del_address(sm, address[0], 1);
              return;
            }
        }
    }
}


int snat_add_interface_address (snat_main_t *sm, u32 sw_if_index, int is_del)
{
  ip4_main_t * ip4_main = sm->ip4_main;
  ip4_address_t * first_int_addr;
  snat_static_map_resolve_t *rp;
  u32 *indices_to_delete = 0;
  int i, j;

  first_int_addr = ip4_interface_first_address (ip4_main, sw_if_index,
                                                0 /* just want the address*/);

  for (i = 0; i < vec_len(sm->auto_add_sw_if_indices); i++)
    {
      if (sm->auto_add_sw_if_indices[i] == sw_if_index)
        {
          if (is_del)
            {
              /* if have address remove it */
              if (first_int_addr)
                  (void) snat_del_address (sm, first_int_addr[0], 1);
              else
                {
                  for (j = 0; j < vec_len (sm->to_resolve); j++)
                    {
                      rp = sm->to_resolve + j;
                      if (rp->sw_if_index == sw_if_index)
                        vec_add1 (indices_to_delete, j);
                    }
                  if (vec_len(indices_to_delete))
                    {
                      for (j = vec_len(indices_to_delete)-1; j >= 0; j--)
                        vec_del1(sm->to_resolve, j);
                      vec_free(indices_to_delete);
                    }
                }
              vec_del1(sm->auto_add_sw_if_indices, i);
            }
          else
            return VNET_API_ERROR_VALUE_EXIST;

          return 0;
        }
    }
  
  if (is_del)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  /* add to the auto-address list */
  vec_add1(sm->auto_add_sw_if_indices, sw_if_index);

  /* If the address is already bound - or static - add it now */
  if (first_int_addr)
      snat_add_address (sm, first_int_addr, ~0);

  return 0;
}

static clib_error_t *
snat_add_interface_address_command_fn (vlib_main_t * vm,
                                       unformat_input_t * input,
                                       vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  u32 sw_if_index;
  int rv;
  int is_del = 0;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U", unformat_vnet_sw_interface,
                    sm->vnet_main, &sw_if_index))
        ;
      else if (unformat (line_input, "del"))
        is_del = 1;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
				     format_unformat_error, line_input);
          goto done;
        }
    }

  rv = snat_add_interface_address (sm, sw_if_index, is_del);

  switch (rv)
    {
    case 0:
      break;

    default:
      error = clib_error_return (0, "snat_add_interface_address returned %d",
                                 rv);
      goto done;
    }

done:
  unformat_free (line_input);

  return error;
}

VLIB_CLI_COMMAND (snat_add_interface_address_command, static) = {
    .path = "snat add interface address",
    .short_help = "snat add interface address <interface> [del]",
    .function = snat_add_interface_address_command_fn,
};

static clib_error_t *
snat_det_map_command_fn (vlib_main_t * vm,
                         unformat_input_t * input,
                         vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  ip4_address_t in_addr, out_addr;
  u32 in_plen, out_plen;
  int is_add = 1, rv;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "in %U/%u", unformat_ip4_address, &in_addr, &in_plen))
        ;
      else if (unformat (line_input, "out %U/%u", unformat_ip4_address, &out_addr, &out_plen))
        ;
      else if (unformat (line_input, "del"))
        is_add = 0;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
                                     format_unformat_error, line_input);
          goto done;
        }
    }

  unformat_free (line_input);

  rv = snat_det_add_map(sm, &in_addr, (u8) in_plen, &out_addr, (u8)out_plen,
                        is_add);

  if (rv)
    {
      error = clib_error_return (0, "snat_det_add_map return %d", rv);
      goto done;
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat deterministic add}
 * Create bijective mapping of inside address to outside address and port range
 * pairs, with the purpose of enabling deterministic NAT to reduce logging in
 * CGN deployments.
 * To create deterministic mapping between inside network 10.0.0.0/18 and
 * outside network 1.1.1.0/30 use:
 * # vpp# snat deterministic add in 10.0.0.0/18 out 1.1.1.0/30
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_det_map_command, static) = {
    .path = "snat deterministic add",
    .short_help = "snat deterministic add in <addr>/<plen> out <addr>/<plen> [del]",
    .function = snat_det_map_command_fn,
};

static clib_error_t *
snat_det_forward_command_fn (vlib_main_t * vm,
                             unformat_input_t * input,
                             vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  ip4_address_t in_addr, out_addr;
  u16 lo_port;
  snat_det_map_t * dm;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U", unformat_ip4_address, &in_addr))
        ;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
                                     format_unformat_error, line_input);
          goto done;
        }
    }

  unformat_free (line_input);

  dm = snat_det_map_by_user(sm, &in_addr);
  if (!dm)
    vlib_cli_output (vm, "no match");
  else
    {
      snat_det_forward (dm, &in_addr, &out_addr, &lo_port);
      vlib_cli_output (vm, "%U:<%d-%d>", format_ip4_address, &out_addr,
                       lo_port, lo_port + dm->ports_per_host - 1);
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat deterministic forward}
 * Return outside address and port range from inside address for deterministic
 * NAT.
 * To obtain outside address and port of inside host use:
 *  vpp# snat deterministic forward 10.0.0.2
 *  1.1.1.0:<1054-1068>
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_det_forward_command, static) = {
    .path = "snat deterministic forward",
    .short_help = "snat deterministic forward <addr>",
    .function = snat_det_forward_command_fn,
};

static clib_error_t *
snat_det_reverse_command_fn (vlib_main_t * vm,
                             unformat_input_t * input,
                             vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  ip4_address_t in_addr, out_addr;
  u32 out_port;
  snat_det_map_t * dm;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U:%d", unformat_ip4_address, &out_addr, &out_port))
        ;
      else
        {
          error =  clib_error_return (0, "unknown input '%U'",
                                      format_unformat_error, line_input);
        }
    }

  unformat_free (line_input);

  if (out_port < 1024 || out_port > 65535)
    {
      error = clib_error_return (0, "wrong port, must be <1024-65535>");
      goto done;
    }

  dm = snat_det_map_by_out(sm, &out_addr);
  if (!dm)
    vlib_cli_output (vm, "no match");
  else
    {
      snat_det_reverse (dm, &out_addr, (u16) out_port, &in_addr);
      vlib_cli_output (vm, "%U", format_ip4_address, &in_addr);
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat deterministic reverse}
 * Return inside address from outside address and port for deterministic NAT.
 * To obtain inside host address from outside address and port use:
 *  #vpp snat deterministic reverse 1.1.1.1:1276
 *  10.0.16.16
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_det_reverse_command, static) = {
    .path = "snat deterministic reverse",
    .short_help = "snat deterministic reverse <addr>:<port>",
    .function = snat_det_reverse_command_fn,
};

static clib_error_t *
set_timeout_command_fn (vlib_main_t * vm,
                        unformat_input_t * input,
                        vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "udp %u", &sm->udp_timeout))
        ;
      else if (unformat (line_input, "tcp-established %u",
               &sm->tcp_established_timeout))
        ;
      else if (unformat (line_input, "tcp-transitory %u",
               &sm->tcp_transitory_timeout))
        ;
      else if (unformat (line_input, "icmp %u", &sm->icmp_timeout))
        ;
      else if (unformat (line_input, "reset"))
        {
          sm->udp_timeout = SNAT_UDP_TIMEOUT;
          sm->tcp_established_timeout = SNAT_TCP_ESTABLISHED_TIMEOUT;
          sm->tcp_transitory_timeout = SNAT_TCP_TRANSITORY_TIMEOUT;
          sm->icmp_timeout = SNAT_ICMP_TIMEOUT;
        }
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
                                     format_unformat_error, line_input);
          goto done;
        }
    }

  unformat_free (line_input);

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{set snat deterministic timeout}
 * Set values of timeouts for deterministic NAT (in seconds), use:
 *  vpp# set snat deterministic timeout udp 120 tcp-established 7500
 *  tcp-transitory 250 icmp 90
 * To reset default values use:
 *  vpp# set snat deterministic timeout reset
 * @cliexend
?*/
VLIB_CLI_COMMAND (set_timeout_command, static) = {
  .path = "set snat deterministic timeout",
  .function = set_timeout_command_fn,
  .short_help =
    "set snat deterministic timeout [udp <sec> | tcp-established <sec> "
    "tcp-transitory <sec> | icmp <sec> | reset]",
};

static clib_error_t *
snat_det_close_session_out_fn (vlib_main_t *vm,
                               unformat_input_t * input,
                               vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  ip4_address_t out_addr, ext_addr, in_addr;
  u16 out_port, ext_port;
  snat_det_map_t * dm;
  snat_det_session_t * ses;
  snat_det_out_key_t key;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U:%d %U:%d",
                    unformat_ip4_address, &out_addr, &out_port,
                    unformat_ip4_address, &ext_addr, &ext_port))
        ;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
                                     format_unformat_error, line_input);
          goto done;
        }
    }

  unformat_free (line_input);

  dm = snat_det_map_by_out(sm, &out_addr);
  if (!dm)
    vlib_cli_output (vm, "no match");
  else
    {
      snat_det_reverse(dm, &ext_addr, out_port, &in_addr);
      key.ext_host_addr = out_addr;
      key.ext_host_port = ntohs(ext_port);
      key.out_port = ntohs(out_port);
      ses = snat_det_get_ses_by_out(dm, &out_addr, key.as_u64);
      if (!ses)
        vlib_cli_output (vm, "no match");
      else
       snat_det_ses_close(dm, ses);
    }

done:
  unformat_free (line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat deterministic close session out}
 * Close session using outside ip address and port
 * and external ip address and port, use:
 *  vpp# snat deterministic close session out 1.1.1.1:1276 2.2.2.2:2387
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_det_close_sesion_out_command, static) = {
  .path = "snat deterministic close session out",
  .short_help = "snat deterministic close session out "
                "<out_addr>:<out_port> <ext_addr>:<ext_port>",
  .function = snat_det_close_session_out_fn,
};

static clib_error_t *
snat_det_close_session_in_fn (vlib_main_t *vm,
                              unformat_input_t * input,
                              vlib_cli_command_t * cmd)
{
  snat_main_t *sm = &snat_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  ip4_address_t in_addr, ext_addr;
  u16 in_port, ext_port;
  snat_det_map_t * dm;
  snat_det_session_t * ses;
  snat_det_out_key_t key;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U:%d %U:%d",
                    unformat_ip4_address, &in_addr, &in_port,
                    unformat_ip4_address, &ext_addr, &ext_port))
        ;
      else
        {
          error = clib_error_return (0, "unknown input '%U'",
                                     format_unformat_error, line_input);
          goto done;
        }
    }

  unformat_free (line_input);

  dm = snat_det_map_by_user (sm, &in_addr);
  if (!dm)
    vlib_cli_output (vm, "no match");
  else
    {
      key.ext_host_addr = ext_addr;
      key.ext_host_port = ntohs (ext_port);
      ses = snat_det_find_ses_by_in (dm, &in_addr, ntohs(in_port), key);
      if (!ses)
        vlib_cli_output (vm, "no match");
      else
        snat_det_ses_close(dm, ses);
    }

done:
  unformat_free(line_input);

  return error;
}

/*?
 * @cliexpar
 * @cliexstart{snat deterministic close_session_in}
 * Close session using inside ip address and port
 * and external ip address and port, use:
 *  vpp# snat deterministic close session in 3.3.3.3:3487 2.2.2.2:2387
 * @cliexend
?*/
VLIB_CLI_COMMAND (snat_det_close_session_in_command, static) = {
  .path = "snat deterministic close session in",
  .short_help = "snat deterministic close session in "
                "<in_addr>:<in_port> <ext_addr>:<ext_port>",
  .function = snat_det_close_session_in_fn,
};
