/*
 * Copyright (c) 2018 Cisco and/or its affiliates.
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

#include <plugins/gbp/gbp_bridge_domain.h>
#include <plugins/gbp/gbp_route_domain.h>
#include <plugins/gbp/gbp_endpoint.h>
#include <plugins/gbp/gbp_learn.h>
#include <plugins/gbp/gbp_itf.h>

#include <vnet/dpo/dvr_dpo.h>
#include <vnet/fib/fib_table.h>
#include <vnet/l2/l2_input.h>
#include <vnet/l2/feat_bitmap.h>
#include <vnet/l2/l2_bvi.h>
#include <vnet/l2/l2_fib.h>

/**
 * Pool of GBP bridge_domains
 */
gbp_bridge_domain_t *gbp_bridge_domain_pool;

/**
 * DB of bridge_domains
 */
gbp_bridge_domain_db_t gbp_bridge_domain_db;

/**
 * Map of BD index to contract scope
 */
gbp_scope_t *gbp_scope_by_bd_index;

/**
 * logger
 */
vlib_log_class_t gb_logger;

#define GBP_BD_DBG(...)                           \
    vlib_log_debug (gb_logger, __VA_ARGS__);

index_t
gbp_bridge_domain_index (const gbp_bridge_domain_t * gbd)
{
  return (gbd - gbp_bridge_domain_pool);
}

static void
gbp_bridge_domain_lock (index_t i)
{
  gbp_bridge_domain_t *gb;

  gb = gbp_bridge_domain_get (i);
  gb->gb_locks++;
}

u32
gbp_bridge_domain_get_bd_id (index_t gbdi)
{
  gbp_bridge_domain_t *gb;

  gb = gbp_bridge_domain_get (gbdi);

  return (gb->gb_bd_id);
}

static index_t
gbp_bridge_domain_find (u32 bd_id)
{
  uword *p;

  p = hash_get (gbp_bridge_domain_db.gbd_by_bd_id, bd_id);

  if (NULL != p)
    return p[0];

  return (INDEX_INVALID);
}

index_t
gbp_bridge_domain_find_and_lock (u32 bd_id)
{
  uword *p;

  p = hash_get (gbp_bridge_domain_db.gbd_by_bd_id, bd_id);

  if (NULL != p)
    {
      gbp_bridge_domain_lock (p[0]);
      return p[0];
    }
  return (INDEX_INVALID);
}

static void
gbp_bridge_domain_db_add (gbp_bridge_domain_t * gb)
{
  index_t gbi = gb - gbp_bridge_domain_pool;

  hash_set (gbp_bridge_domain_db.gbd_by_bd_id, gb->gb_bd_id, gbi);
  vec_validate_init_empty (gbp_bridge_domain_db.gbd_by_bd_index,
			   gb->gb_bd_index, INDEX_INVALID);
  gbp_bridge_domain_db.gbd_by_bd_index[gb->gb_bd_index] = gbi;
}

static void
gbp_bridge_domain_db_remove (gbp_bridge_domain_t * gb)
{
  hash_unset (gbp_bridge_domain_db.gbd_by_bd_id, gb->gb_bd_id);
  gbp_bridge_domain_db.gbd_by_bd_index[gb->gb_bd_index] = INDEX_INVALID;
}

u8 *
format_gbp_bridge_domain_flags (u8 * s, va_list * args)
{
  gbp_bridge_domain_flags_t gf = va_arg (*args, gbp_bridge_domain_flags_t);

  if (gf)
    {
      if (gf & GBP_BD_FLAG_DO_NOT_LEARN)
	s = format (s, "do-not-learn ");
      if (gf & GBP_BD_FLAG_UU_FWD_DROP)
	s = format (s, "uu-fwd-drop ");
      if (gf & GBP_BD_FLAG_MCAST_DROP)
	s = format (s, "mcast-drop ");
      if (gf & GBP_BD_FLAG_UCAST_ARP)
	s = format (s, "ucast-arp ");
    }
  else
    {
      s = format (s, "none");
    }
  return (s);
}

static u8 *
format_gbp_bridge_domain_ptr (u8 * s, va_list * args)
{
  gbp_bridge_domain_t *gb = va_arg (*args, gbp_bridge_domain_t *);
  vnet_main_t *vnm = vnet_get_main ();

  if (NULL != gb)
    s =
      format (s,
	      "[%d] bd:[%d,%d], bvi:%U uu-flood:%U bm-flood:%U flags:%U locks:%d",
	      gb - gbp_bridge_domain_pool, gb->gb_bd_id, gb->gb_bd_index,
	      format_vnet_sw_if_index_name, vnm, gb->gb_bvi_sw_if_index,
	      format_vnet_sw_if_index_name, vnm, gb->gb_uu_fwd_sw_if_index,
	      format_gbp_itf_hdl, gb->gb_bm_flood_itf,
	      format_gbp_bridge_domain_flags, gb->gb_flags, gb->gb_locks);
  else
    s = format (s, "NULL");

  return (s);
}

u8 *
format_gbp_bridge_domain (u8 * s, va_list * args)
{
  index_t gbi = va_arg (*args, index_t);

  s =
    format (s, "%U", format_gbp_bridge_domain_ptr,
	    gbp_bridge_domain_get (gbi));

  return (s);
}

int
gbp_bridge_domain_add_and_lock (u32 bd_id,
				u32 rd_id,
				gbp_bridge_domain_flags_t flags,
				u32 bvi_sw_if_index,
				u32 uu_fwd_sw_if_index,
				u32 bm_flood_sw_if_index)
{
  gbp_bridge_domain_t *gb;
  index_t gbi;

  gbi = gbp_bridge_domain_find (bd_id);

  if (INDEX_INVALID == gbi)
    {
      gbp_route_domain_t *gr;
      u32 bd_index;

      bd_index = bd_find_index (&bd_main, bd_id);

      if (~0 == bd_index)
	return (VNET_API_ERROR_BD_NOT_MODIFIABLE);

      bd_flags_t bd_flags = L2_NONE;
      if (flags & GBP_BD_FLAG_UU_FWD_DROP)
	bd_flags |= L2_UU_FLOOD;
      if (flags & GBP_BD_FLAG_MCAST_DROP)
	bd_flags |= L2_FLOOD;

      pool_get (gbp_bridge_domain_pool, gb);
      memset (gb, 0, sizeof (*gb));

      gbi = gb - gbp_bridge_domain_pool;
      gb->gb_bd_id = bd_id;
      gb->gb_bd_index = bd_index;
      gb->gb_uu_fwd_sw_if_index = uu_fwd_sw_if_index;
      gb->gb_bvi_sw_if_index = bvi_sw_if_index;
      gbp_itf_hdl_reset (&gb->gb_bm_flood_itf);
      gb->gb_locks = 1;
      gb->gb_flags = flags;
      gb->gb_rdi = gbp_route_domain_find_and_lock (rd_id);

      /*
       * set the scope from the BD's RD's scope
       */
      gr = gbp_route_domain_get (gb->gb_rdi);
      vec_validate (gbp_scope_by_bd_index, gb->gb_bd_index);
      gbp_scope_by_bd_index[gb->gb_bd_index] = gr->grd_scope;

      /*
       * Set the BVI and uu-flood interfaces into the BD
       */
      gbp_bridge_domain_itf_add (gbi, gb->gb_bvi_sw_if_index,
				 L2_BD_PORT_TYPE_BVI);

      if ((!(flags & GBP_BD_FLAG_UU_FWD_DROP) ||
	   (flags & GBP_BD_FLAG_UCAST_ARP)) &&
	  ~0 != gb->gb_uu_fwd_sw_if_index)
	gbp_bridge_domain_itf_add (gbi, gb->gb_uu_fwd_sw_if_index,
				   L2_BD_PORT_TYPE_UU_FWD);

      if (!(flags & GBP_BD_FLAG_MCAST_DROP) && ~0 != bm_flood_sw_if_index)
	{
	  gb->gb_bm_flood_itf =
	    gbp_itf_l2_add_and_lock (bm_flood_sw_if_index, gbi);
	  gbp_itf_l2_set_input_feature (gb->gb_bm_flood_itf,
					L2INPUT_FEAT_GBP_LEARN);
	}

      /*
       * unset any flag(s) set above
       */
      bd_set_flags (vlib_get_main (), bd_index, bd_flags, 0);

      if (flags & GBP_BD_FLAG_UCAST_ARP)
	{
	  bd_flags = L2_ARP_UFWD;
	  bd_set_flags (vlib_get_main (), bd_index, bd_flags, 1);
	}

      /*
       * Add the BVI's MAC to the L2FIB
       */
      l2fib_add_entry (vnet_sw_interface_get_hw_address
		       (vnet_get_main (), gb->gb_bvi_sw_if_index),
		       gb->gb_bd_index, gb->gb_bvi_sw_if_index,
		       (L2FIB_ENTRY_RESULT_FLAG_STATIC |
			L2FIB_ENTRY_RESULT_FLAG_BVI));

      gbp_bridge_domain_db_add (gb);
    }
  else
    {
      gb = gbp_bridge_domain_get (gbi);
      gb->gb_locks++;
    }

  GBP_BD_DBG ("add: %U", format_gbp_bridge_domain_ptr, gb);

  return (0);
}

void
gbp_bridge_domain_itf_add (index_t gbdi,
			   u32 sw_if_index, l2_bd_port_type_t type)
{
  gbp_bridge_domain_t *gb;

  gb = gbp_bridge_domain_get (gbdi);

  set_int_l2_mode (vlib_get_main (), vnet_get_main (), MODE_L2_BRIDGE,
		   sw_if_index, gb->gb_bd_index, type, 0, 0);
  /*
   * adding an interface to the bridge enables learning on the
   * interface. Disable learning on the interface by default for gbp
   * interfaces
   */
  l2input_intf_bitmap_enable (sw_if_index, L2INPUT_FEAT_LEARN, 0);
}

void
gbp_bridge_domain_itf_del (index_t gbdi,
			   u32 sw_if_index, l2_bd_port_type_t type)
{
  gbp_bridge_domain_t *gb;

  gb = gbp_bridge_domain_get (gbdi);

  set_int_l2_mode (vlib_get_main (), vnet_get_main (), MODE_L3, sw_if_index,
		   gb->gb_bd_index, type, 0, 0);
}

void
gbp_bridge_domain_unlock (index_t gbdi)
{
  gbp_bridge_domain_t *gb;

  gb = gbp_bridge_domain_get (gbdi);

  gb->gb_locks--;

  if (0 == gb->gb_locks)
    {
      GBP_BD_DBG ("destroy: %U", format_gbp_bridge_domain_ptr, gb);

      l2fib_del_entry (vnet_sw_interface_get_hw_address
		       (vnet_get_main (), gb->gb_bvi_sw_if_index),
		       gb->gb_bd_index, gb->gb_bvi_sw_if_index);

      gbp_bridge_domain_itf_del (gbdi, gb->gb_bvi_sw_if_index,
				 L2_BD_PORT_TYPE_BVI);
      if (~0 != gb->gb_uu_fwd_sw_if_index)
	gbp_bridge_domain_itf_del (gbdi, gb->gb_uu_fwd_sw_if_index,
				   L2_BD_PORT_TYPE_UU_FWD);
      gbp_itf_unlock (&gb->gb_bm_flood_itf);

      gbp_bridge_domain_db_remove (gb);
      gbp_route_domain_unlock (gb->gb_rdi);

      pool_put (gbp_bridge_domain_pool, gb);
    }
}

int
gbp_bridge_domain_delete (u32 bd_id)
{
  index_t gbi;

  GBP_BD_DBG ("del: %d", bd_id);
  gbi = gbp_bridge_domain_find (bd_id);

  if (INDEX_INVALID != gbi)
    {
      GBP_BD_DBG ("del: %U", format_gbp_bridge_domain, gbi);
      gbp_bridge_domain_unlock (gbi);

      return (0);
    }

  return (VNET_API_ERROR_NO_SUCH_ENTRY);
}

void
gbp_bridge_domain_walk (gbp_bridge_domain_cb_t cb, void *ctx)
{
  gbp_bridge_domain_t *gbpe;

  /* *INDENT-OFF* */
  pool_foreach (gbpe, gbp_bridge_domain_pool)
  {
    if (!cb(gbpe, ctx))
      break;
  }
  /* *INDENT-ON* */
}

static clib_error_t *
gbp_bridge_domain_cli (vlib_main_t * vm,
		       unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  gbp_bridge_domain_flags_t flags;
  u32 bm_flood_sw_if_index = ~0;
  u32 uu_fwd_sw_if_index = ~0;
  u32 bd_id = ~0, rd_id = ~0;
  u32 bvi_sw_if_index = ~0;
  u8 add = 1;

  flags = GBP_BD_FLAG_NONE;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "bvi %U", unformat_vnet_sw_interface,
		    vnm, &bvi_sw_if_index))
	;
      else if (unformat (input, "uu-fwd %U", unformat_vnet_sw_interface,
			 vnm, &uu_fwd_sw_if_index))
	;
      else if (unformat (input, "bm-flood %U", unformat_vnet_sw_interface,
			 vnm, &bm_flood_sw_if_index))
	;
      else if (unformat (input, "add"))
	add = 1;
      else if (unformat (input, "del"))
	add = 0;
      else if (unformat (input, "flags %d", &flags))
	;
      else if (unformat (input, "bd %d", &bd_id))
	;
      else if (unformat (input, "rd %d", &rd_id))
	;
      else
	break;
    }

  if (~0 == bd_id)
    return clib_error_return (0, "BD-ID must be specified");
  if (~0 == rd_id)
    return clib_error_return (0, "RD-ID must be specified");

  if (add)
    {
      if (~0 == bvi_sw_if_index)
	return clib_error_return (0, "interface must be specified");

      gbp_bridge_domain_add_and_lock (bd_id, rd_id,
				      flags,
				      bvi_sw_if_index,
				      uu_fwd_sw_if_index,
				      bm_flood_sw_if_index);
    }
  else
    gbp_bridge_domain_delete (bd_id);

  return (NULL);
}

/*?
 * Configure a GBP bridge-domain
 *
 * @cliexpar
 * @cliexstart{gbp bridge-domain [del] bd <ID> bvi <interface> [uu-fwd <interface>] [bm-flood <interface>] [flags <flags>]}
 * @cliexend
 ?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (gbp_bridge_domain_cli_node, static) = {
  .path = "gbp bridge-domain",
  .short_help = "gbp bridge-domain [del] bd <ID> bvi <interface> [uu-fwd <interface>] [bm-flood <interface>] [flags <flags>]",
  .function = gbp_bridge_domain_cli,
};

static int
gbp_bridge_domain_show_one (gbp_bridge_domain_t *gb, void *ctx)
{
  vlib_main_t *vm;

  vm = ctx;
  vlib_cli_output (vm, "  %U", format_gbp_bridge_domain_ptr, gb);

  return (1);
}

static clib_error_t *
gbp_bridge_domain_show (vlib_main_t * vm,
		   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vlib_cli_output (vm, "Bridge-Domains:");
  gbp_bridge_domain_walk (gbp_bridge_domain_show_one, vm);

  return (NULL);
}


/*?
 * Show Group Based Policy Bridge_Domains and derived information
 *
 * @cliexpar
 * @cliexstart{show gbp bridge_domain}
 * @cliexend
 ?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (gbp_bridge_domain_show_node, static) = {
  .path = "show gbp bridge-domain",
  .short_help = "show gbp bridge-domain\n",
  .function = gbp_bridge_domain_show,
};
/* *INDENT-ON* */

static clib_error_t *
gbp_bridge_domain_init (vlib_main_t * vm)
{
  gb_logger = vlib_log_register_class ("gbp", "bd");

  return (NULL);
}

VLIB_INIT_FUNCTION (gbp_bridge_domain_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
