#include <bts/blockchain/chain_database.hpp>
#include <bts/blockchain/config.hpp>
#include <bts/blockchain/genesis_config.hpp>
#include <bts/blockchain/time.hpp>
#include <bts/blockchain/operation_factory.hpp>
#include <bts/blockchain/fire_operation.hpp>

#include <bts/db/level_map.hpp>
#include <bts/db/level_pod_map.hpp>

#include <fc/io/json.hpp>
#include <fc/io/raw_variant.hpp>
#include <fc/io/fstream.hpp>
#include <fc/log/logger.hpp>

#include <fstream>
#include <iostream>

using namespace bts::blockchain;

struct vote_del
{
   vote_del( int64_t v = 0, account_id_type del = 0 )
   :votes(v),delegate_id(del){}
   int64_t votes;
   account_id_type delegate_id;
   friend bool operator == ( const vote_del& a, const vote_del& b )
   {
      return a.votes == b.votes && a.delegate_id == b.delegate_id;
   }
   friend bool operator < ( const vote_del& a, const vote_del& b )
   {
      if( a.votes > b.votes ) return true;
      if( a.votes < b.votes ) return false;
      return a.delegate_id < b.delegate_id; /* Lowest id wins in ties */
   }
};
FC_REFLECT( vote_del, (votes)(delegate_id) )

struct fee_index
{
   fee_index( share_type fees = 0, transaction_id_type trx = transaction_id_type() )
   :_fees(fees),_trx(trx){}
   share_type          _fees;
   transaction_id_type _trx;
   friend bool operator == ( const fee_index& a, const fee_index& b )
   {
      return a._fees == b._fees && a._trx == b._trx;
   }
   friend bool operator < ( const fee_index& a, const fee_index& b )
   {
      if( a._fees == b._fees ) return a._trx < b._trx; /* Lowest id wins in ties */
      return a._fees > b._fees; /* Reverse so that highest fee is placed first in sorted maps */
   }
};
FC_REFLECT( fee_index, (_fees)(_trx) )

struct block_fork_data
{
   block_fork_data():is_linked(false),is_included(false){}

   bool invalid()const
   {
      if( !!is_valid ) return !*is_valid;
      return false;
   }
   bool valid()const
   {
      if( !!is_valid ) return *is_valid;
      return false;
   }
   bool can_link()const
   {
      return is_linked && !invalid();
   }

   std::unordered_set<block_id_type> next_blocks; ///< IDs of all blocks that come after
   bool                              is_linked;   ///< is linked to genesis block

   /** if at any time this block was determiend to be valid or invalid then this
    * flag will be set.
    */
   fc::optional<bool>         is_valid;
   bool                       is_included; ///< is included in the current chain database
};
FC_REFLECT( block_fork_data, (next_blocks)(is_linked)(is_valid)(is_included) )
FC_REFLECT_TYPENAME( std::vector<bts::blockchain::block_id_type> )


namespace bts { namespace blockchain {

   namespace detail
   {

      class chain_database_impl
      {
         public:
            chain_database_impl():self(nullptr),_observer(nullptr){}

            void                       initialize_genesis(fc::path genesis_file);


            block_fork_data            store_and_index( const block_id_type& id, const full_block& blk );
            void                       clear_pending(  const full_block& blk );
            void                       switch_to_fork( const block_id_type& block_id );
            void                       extend_chain( const full_block& blk );
            std::vector<block_id_type> get_fork_history( const block_id_type& id );
            void                       pop_block();
            void                       mark_invalid( const block_id_type& id );
            void                       mark_included( const block_id_type& id, bool state );
            void                       verify_header( const full_block& );
            void                       apply_transactions( uint32_t block_num,
                                                           const std::vector<signed_transaction>&,
                                                           const pending_chain_state_ptr& );
            void                       pay_delegate( fc::time_point_sec time_slot, share_type amount,
                                                           const pending_chain_state_ptr& );
            void                       save_undo_state( const block_id_type& id,
                                                           const pending_chain_state_ptr& );
            void                       update_head_block( const full_block& blk );
            std::vector<block_id_type> fetch_blocks_at_number( uint32_t block_num );
            void                       recursive_mark_as_linked( const std::unordered_set<block_id_type>& ids );
            void                       recursive_mark_as_invalid( const std::unordered_set<block_id_type>& ids );
            void                       update_random_seed( secret_hash_type new_secret, 
                                                          const pending_chain_state_ptr& pending_state );
            void                       update_active_delegate_list(const full_block& block_data, const pending_chain_state_ptr& pending_state );


            void                       update_delegate_production_info( const full_block& block_data, 
                                                                        const pending_chain_state_ptr& pending_state );

            chain_database*                                                     self;
            chain_observer*                                                     _observer;
            digest_type                                                         _chain_id;

            bts::db::level_map<uint32_t, std::vector<block_id_type> >           _fork_number_db;
            bts::db::level_map<block_id_type,block_fork_data>                   _fork_db;
            bts::db::level_map<uint32_t, fc::variant >                          _property_db;
            bts::db::level_map<proposal_id_type, proposal_record >              _proposal_db;
            bts::db::level_map<proposal_vote_id_type, proposal_vote >           _proposal_vote_db;

            /** the data required to 'undo' the changes a block made to the database */
            bts::db::level_map<block_id_type,pending_chain_state>               _undo_state_db;

            // blocks in the current 'official' chain.
            bts::db::level_map<uint32_t,block_id_type>                          _block_num_to_id_db;
            // all blocks from any fork..
            bts::db::level_map<block_id_type,full_block>                        _block_id_to_block_db;

            // used to revert block state in the event of a fork
            // bts::db::level_map<uint32_t,undo_data>                              _block_num_to_undo_data_db;

            signed_block_header                                                 _head_block_header;
            block_id_type                                                       _head_block_id;

            bts::db::level_map< transaction_id_type, signed_transaction>        _pending_transaction_db;
            std::map< fee_index, transaction_evaluation_state_ptr >             _pending_fee_index;


            bts::db::level_map< asset_id_type, asset_record >                   _asset_db;
            bts::db::level_map< balance_id_type, balance_record >               _balance_db;
            bts::db::level_map< account_id_type, account_record >               _account_db;
            bts::db::level_map< address, account_id_type >                      _address_to_account_db;


            bts::db::level_map< string, account_id_type >                       _account_index_db;
            bts::db::level_map< string, asset_id_type >                         _symbol_index_db;
            bts::db::level_pod_map< vote_del, int >                             _delegate_vote_index_db;


            bts::db::level_map< market_index_key, order_record >                _ask_db;
            bts::db::level_map< market_index_key, order_record >                _bid_db;
            bts::db::level_map< market_index_key, order_record >                _short_db;
            bts::db::level_map< market_index_key, collateral_record >           _collateral_db;

            /** used to prevent duplicate processing */
            bts::db::level_pod_map< transaction_id_type, transaction_location > _processed_transaction_id_db;
      };

      std::vector<block_id_type> chain_database_impl::fetch_blocks_at_number( uint32_t block_num )
      {
         std::vector<block_id_type> current_blocks;
         auto itr = _fork_number_db.find( block_num );
         if( itr.valid() ) return itr.value();
         return current_blocks;
      }

      void  chain_database_impl::clear_pending(  const full_block& blk )
      {
         std::unordered_set<transaction_id_type> confirmed_trx_ids;

         for( auto trx : blk.user_transactions )
         {
            auto id = trx.id();
            confirmed_trx_ids.insert( id );
            _pending_transaction_db.remove( id );
         }

         auto temp_pending_fee_index( _pending_fee_index );
         for( auto pair : temp_pending_fee_index )
         {
            auto fee_index = pair.first;

            if( confirmed_trx_ids.count( fee_index._trx ) > 0 )
               _pending_fee_index.erase( fee_index );
         }
      }

      void chain_database_impl::recursive_mark_as_linked( const std::unordered_set<block_id_type>& ids )
      {
         std::unordered_set<block_id_type> next_ids = ids;
         while( next_ids.size() )
         {
            std::unordered_set<block_id_type> pending;
            for( auto item : next_ids )
            {
                block_fork_data record = _fork_db.fetch( item );
                record.is_linked = true;
                pending.insert( record.next_blocks.begin(), record.next_blocks.end() );
                //ilog( "store: ${id} => ${data}", ("id",item)("data",record) );
                _fork_db.store( item, record );
            }
            next_ids = pending;
         }
      }
      void chain_database_impl::recursive_mark_as_invalid( const std::unordered_set<block_id_type>& ids )
      {
         std::unordered_set<block_id_type> next_ids = ids;
         while( next_ids.size() )
         {
            std::unordered_set<block_id_type> pending;
            for( auto item : next_ids )
            {
                block_fork_data record = _fork_db.fetch( item );
                record.is_valid = false;
                pending.insert( record.next_blocks.begin(), record.next_blocks.end() );
                //ilog( "store: ${id} => ${data}", ("id",item)("data",record) );
                _fork_db.store( item, record );
            }
            next_ids = pending;
         }
      }

      /**
       *  Place the block in the block tree, the block tree contains all blocks
       *  and tracks whether they are valid, linked, and current.
       *
       *  There are several options for this block:
       *
       *  1) It extends an existing block
       *      - a valid chain
       *      - an invalid chain
       *      - an unlinked chain
       *  2) It is free floating and doesn't link to anything we have
       *      - create two entries into the database
       *          - one for this block
       *          - placeholder for previous
       *      - mark both as unlinked
       *  3) It it provides the missing link between a the genesis block and existing chain
       *      - all next blocks need to be updated to change state to 'linked'
       */
      block_fork_data chain_database_impl::store_and_index( const block_id_type& block_id,
                                                            const full_block& block_data )
      { try {
          //ilog( "block_number: ${n}   id: ${id}  prev: ${prev}",
           //     ("n",block_data.block_num)("id",block_id)("prev",block_data.previous) );

          // first of all store this block at the given block number
          _block_id_to_block_db.store( block_id, block_data );

          // update the parallel block list
          std::vector<block_id_type> parallel_blocks = fetch_blocks_at_number( block_data.block_num );
          std::find( parallel_blocks.begin(), parallel_blocks.end(), block_id );
          parallel_blocks.push_back( block_id );
          _fork_number_db.store( block_data.block_num, parallel_blocks );


          // now find how it links in.
          block_fork_data prev_fork_data;
          auto prev_itr = _fork_db.find( block_data.previous );
          if( prev_itr.valid() ) // we already know about its previous
          {
             ilog( "           we already know about its previous: ${p}", ("p",block_data.previous) );
             prev_fork_data = prev_itr.value();
             prev_fork_data.next_blocks.insert(block_id);
             //ilog( "              ${id} = ${record}", ("id",prev_itr.key())("record",prev_fork_data) );
             _fork_db.store( prev_itr.key(), prev_fork_data );
          }
          else
          {
             ilog( "           we don't know about its previous: ${p}", ("p",block_data.previous) );
             // create it... we do not know about the previous block so
             // we must create it and assume it is not linked...
             prev_fork_data.next_blocks.insert(block_id);
             prev_fork_data.is_linked = block_data.previous == block_id_type(); //false;
             //ilog( "              ${id} = ${record}", ("id",block_data.previous)("record",prev_fork_data) );
             _fork_db.store( block_data.previous, prev_fork_data );
          }

          block_fork_data current_fork;
          auto cur_itr = _fork_db.find( block_id );
          if( cur_itr.valid() )
          {
             current_fork = cur_itr.value();
             ilog( "          current_fork: ${fork}", ("fork",current_fork) );
             ilog( "          prev_fork: ${prev_fork}", ("prev_fork",prev_fork_data) );
             if( !current_fork.is_linked && prev_fork_data.is_linked )
             {
                // we found the missing link
                current_fork.is_linked = true;
                recursive_mark_as_linked( current_fork.next_blocks );
                _fork_db.store( block_id, current_fork );
             }
          }
          else
          {
             current_fork.is_linked = prev_fork_data.is_linked;
             //ilog( "          current_fork: ${id} = ${fork}", ("id",block_id)("fork",current_fork) );
             _fork_db.store( block_id, current_fork );
          }
          return current_fork;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id) ) }

      void chain_database_impl::mark_invalid( const block_id_type& block_id )
      {
         // fetch the fork data for block_id, mark it as invalid and
         // then mark every item after it as invalid as well.
         auto fork_data = _fork_db.fetch( block_id );
         fork_data.is_valid = false;
         _fork_db.store( block_id, fork_data );
         recursive_mark_as_invalid( fork_data.next_blocks );
      }

      void chain_database_impl::mark_included( const block_id_type& block_id, bool included )
      { try {
         //ilog( "included: ${block_id} = ${state}", ("block_id",block_id)("state",included) );
         auto fork_data = _fork_db.fetch( block_id );
       //  if( fork_data.is_included != included )
         {
            fork_data.is_included = included;
            if( included )
            {
               fork_data.is_valid  = true;
            }
            //ilog( "store: ${id} => ${data}", ("id",block_id)("data",fork_data) );
            _fork_db.store( block_id, fork_data );
         }
         // fetch the fork data for block_id, mark it as included and
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id)("included",included) ) }

      void chain_database_impl::switch_to_fork( const block_id_type& block_id )
      { try {
         ilog( "switch from fork ${id} to ${to_id}", ("id",_head_block_id)("to_id",block_id) );
         std::vector<block_id_type> history = get_fork_history( block_id );
         FC_ASSERT( history.size() > 0 );
         while( history.back() != _head_block_id )
         {
            ilog( "    pop ${id}", ("id",_head_block_id) );
            pop_block();
         }
         for( int32_t i = history.size()-2; i >= 0 ; --i )
         {
            ilog( "    extend ${i}", ("i",history[i]) );
            extend_chain( self->get_block( history[i] ) );
         }
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id) ) }


      void chain_database_impl::apply_transactions( uint32_t block_num,
                                                    const std::vector<signed_transaction>& user_transactions,
                                                    const pending_chain_state_ptr& pending_state )
      {
         //ilog( "apply transactions ${block_num}", ("block_num",block_num) );
         uint32_t trx_num = 0;
         try {
            // apply changes from each transaction
            for( auto trx : user_transactions )
            {
               transaction_evaluation_state_ptr trx_eval_state =
                      std::make_shared<transaction_evaluation_state>(pending_state,_chain_id);
               trx_eval_state->evaluate( trx );
               //ilog( "evaluation: ${e}", ("e",*trx_eval_state) );
              // TODO:  capture the evaluation state with a callback for wallets...
              // summary.transaction_states.emplace_back( std::move(trx_eval_state) );

               transaction_location trx_loc( block_num, trx_num );
               //ilog( "store trx location: ${loc}", ("loc",trx_loc) );
               pending_state->store_transaction_location( trx.id(), trx_loc );
               ++trx_num;
            }
      } FC_RETHROW_EXCEPTIONS( warn, "", ("trx_num",trx_num) ) }

      void chain_database_impl::pay_delegate(  fc::time_point_sec time_slot, 
                                               share_type amount, 
                                               const pending_chain_state_ptr& pending_state )
      { try {
            auto delegate_record = pending_state->get_account_record( self->get_signing_delegate_id( time_slot ) );
            FC_ASSERT( !!delegate_record );
            FC_ASSERT( delegate_record->is_delegate() );
            delegate_record->delegate_info->pay_balance += amount;
            delegate_record->delegate_info->votes_for += amount;
            pending_state->store_account_record( *delegate_record );

            auto base_asset_record = pending_state->get_asset_record( asset_id_type(0) );
            FC_ASSERT( base_asset_record.valid() );
            base_asset_record->current_share_supply += amount;
            pending_state->store_asset_record( *base_asset_record );

      } FC_RETHROW_EXCEPTIONS( warn, "", ("time_slot",time_slot)("amount",amount) ) }

      void chain_database_impl::save_undo_state( const block_id_type& block_id,
                                               const pending_chain_state_ptr& pending_state )
      { try {
           pending_chain_state_ptr undo_state = std::make_shared<pending_chain_state>(nullptr);
           pending_state->get_undo_state( undo_state );
           _undo_state_db.store( block_id, *undo_state );
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id) ) }


      void chain_database_impl::verify_header( const full_block& block_data )
      { try {
            // validate preliminaries:
            FC_ASSERT( block_data.block_num == _head_block_header.block_num + 1 );
            FC_ASSERT( block_data.previous  == _head_block_id );
            FC_ASSERT( block_data.timestamp.sec_since_epoch() % BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC == 0 );
            FC_ASSERT( block_data.timestamp > _head_block_header.timestamp, "",
                       ("block_data.timestamp",block_data.timestamp)("timestamp()",_head_block_header.timestamp)  );
            fc::time_point_sec now = bts::blockchain::now();
            FC_ASSERT( block_data.timestamp <=  (now + BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC/2),
                       "${t} < ${now}", ("t",block_data.timestamp)("now",now));

            size_t block_size = block_data.block_size();
            auto   expected_next_fee = block_data.next_fee( self->get_fee_rate(),  block_size );

            FC_ASSERT( block_data.fee_rate  == expected_next_fee );

            digest_block digest_data(block_data);
            FC_ASSERT( digest_data.validate_digest() );
            FC_ASSERT( digest_data.validate_unique() );

            // signign delegate id: 
            auto signing_delegate_id = self->get_signing_delegate_id( block_data.timestamp );
            FC_ASSERT( block_data.validate_signee( self->get_signing_delegate_key(block_data.timestamp) ),
                       "", ("signing_delegate_key", self->get_signing_delegate_key(block_data.timestamp))
                           ("signing_delegate_id", signing_delegate_id ) );

      } FC_RETHROW_EXCEPTIONS( warn, "" ) }

      void chain_database_impl::update_head_block( const full_block& block_data )
      {
         _head_block_header = block_data;
         _head_block_id = block_data.id();
      }

      /**
       *  A block should be produced every BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC. If we do not have
       *  a block for any multiple of this interval between block_data and the current head block then
       *  we need to lookup the delegates that should have produced a block at that interval and then
       *  increment their missed block count.
       *
       *  Then we need to increment the produced_block count for the delegate that produced block_data.
       *
       *  Note that the header of block_data has already been verified by the caller and that updates
       *  are applied to pending_state.
       */
      void chain_database_impl::update_delegate_production_info( const full_block& produced_block,
                                                                 const pending_chain_state_ptr& pending_state )
      {
          // validate secret
          {
             auto delegate_id = self->get_signing_delegate_id( produced_block.timestamp );
             auto delegate_rec = pending_state->get_account_record( delegate_id );

             if( delegate_rec->delegate_info->blocks_produced > 0 )
             {
                 auto hash_of_previous_secret = fc::ripemd160::hash(produced_block.previous_secret); 
                 FC_ASSERT( hash_of_previous_secret == delegate_rec->delegate_info->next_secret_hash,
                            "",
                            ("previous_secret",produced_block.previous_secret)
                            ("hash_of_previous_secret",hash_of_previous_secret)
                            ("delegate",*delegate_rec) );
             }

             delegate_rec->delegate_info->next_secret_hash         = produced_block.next_secret_hash;
             delegate_rec->delegate_info->last_block_num_produced  = produced_block.block_num;
             pending_state->store_account_record( *delegate_rec );
          }

          auto headblock_timestamp = _head_block_header.timestamp;
          if( _head_block_header.block_num == 0 )
          {
              headblock_timestamp = produced_block.timestamp;
              headblock_timestamp -= BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
          }

          do
          {
              headblock_timestamp += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;

              auto delegate_id = self->get_signing_delegate_id( headblock_timestamp );
              auto delegate_rec = pending_state->get_account_record( delegate_id );

              if( headblock_timestamp != produced_block.timestamp )
                  delegate_rec->delegate_info->blocks_missed += 1;
              else
                  delegate_rec->delegate_info->blocks_produced += 1;

              pending_state->store_account_record( *delegate_rec );
          }
          while( headblock_timestamp != produced_block.timestamp );
      }
      void chain_database_impl::update_random_seed( secret_hash_type new_secret, 
                                                    const pending_chain_state_ptr& pending_state )
      {
         auto current_seed = pending_state->get_current_random_seed();
         fc::sha512::encoder enc;
         fc::raw::pack( enc, new_secret );
         fc::raw::pack( enc, current_seed );
         pending_state->set_property( last_random_seed_id, 
                                      fc::variant(fc::ripemd160::hash( enc.result() )) );
      }
       
      void chain_database_impl::update_active_delegate_list( const full_block& block_data, 
                                                             const pending_chain_state_ptr& pending_state )
      {
          if( block_data.block_num % BTS_BLOCKCHAIN_NUM_DELEGATES == 0 )
          {
             // perform a random shuffle of the sorted delegate list.
             
             auto active_del = self->next_round_active_delegates();
             auto rand_seed = fc::sha256::hash(self->get_current_random_seed());
             size_t num_del = active_del.size();
             for( uint32_t i = 0; i < num_del; ++i )
             {
                for( uint32_t x = 0; x < 4 && i < num_del; ++x, ++i )
                   std::swap( active_del[i], active_del[rand_seed._hash[x]%num_del] );
                rand_seed = fc::sha256::hash(rand_seed);
             }

             pending_state->set_property( chain_property_enum::active_delegate_list_id, fc::variant(active_del) );
          }
      }

      /**
       *  Performs all of the block validation steps and throws if error.
       */
      void chain_database_impl::extend_chain( const full_block& block_data )
      { try {
         auto block_id = block_data.id();
         block_summary summary;
         try {
            verify_header( block_data );

            summary.block_data = block_data;

            /* Create a pending state to track changes that would apply as we evaluate the block */
            pending_chain_state_ptr pending_state = std::make_shared<pending_chain_state>(self->shared_from_this());
            summary.applied_changes = pending_state;

            /** Increment the blocks produced or missed for all delegates. This must be done
             *  before applying transactions because it depends upon the current order.
             **/
            update_delegate_production_info( block_data, pending_state );

            // apply any deterministic operations such as market operations before we preterb indexes
            //apply_deterministic_updates(pending_state);

            //ilog( "block data: ${block_data}", ("block_data",block_data) );
            apply_transactions( block_data.block_num, block_data.user_transactions, pending_state );

            pay_delegate( block_data.timestamp, block_data.delegate_pay_rate, pending_state );

            update_active_delegate_list(block_data, pending_state);

            update_random_seed( block_data.previous_secret, pending_state );

            save_undo_state( block_id, pending_state );

            // TODO: verify that apply changes can be called any number of
            // times without changing the database other than the first
            // attempt.
            // ilog( "apply changes\n${s}", ("s",fc::json::to_pretty_string( *pending_state) ) );
            pending_state->apply_changes();

            mark_included( block_id, true );

            update_head_block( block_data );

            clear_pending( block_data );

            _block_num_to_id_db.store( block_data.block_num, block_id );

            self->sanity_check();
         }
         catch ( const fc::exception& e )
         {
            wlog( "error applying block: ${e}", ("e",e.to_detail_string() ));
            mark_invalid( block_id );
            throw;
         }
         if( _observer ) try { 
            _observer->block_applied( summary );
         } catch ( const fc::exception& e )
         {
            wlog( "${e}", ("e",e.to_detail_string() ) );
         }
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block",block_data) ) }

      /**
       * Traverse the previous links of all blocks in fork until we find one that is_included
       *
       * The last item in the result will be the only block id that is already included in
       * the blockchain.  
       */
      std::vector<block_id_type> chain_database_impl::get_fork_history( const block_id_type& id )
      { try {
         ilog( "" );
         std::vector<block_id_type> history;
         history.push_back( id );

         block_id_type next_id = id;
         while( true )
         {
            auto header = self->get_block_header( next_id );
            //ilog( "header: ${h}", ("h",header) );
            history.push_back( header.previous );
            if( header.previous == block_id_type() )
            {
               ilog( "return: ${h}", ("h",history) );
               return history;
            }
            auto prev_fork_data = _fork_db.fetch( header.previous );

            /// this shouldn't happen if the database invariants are properly maintained 
            FC_ASSERT( prev_fork_data.is_linked, "we hit a dead end, this fork isn't really linked!" );
            if( prev_fork_data.is_included )
            {
               ilog( "return: ${h}", ("h",history) );
               return history;
            }
            next_id = header.previous;
         }
         ilog( "${h}", ("h",history) );
         return history;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",id) ) }

      void  chain_database_impl::pop_block()
      { try {
         FC_ASSERT( _head_block_header.block_num > 0 );

         // update the is_included flag on the fork data
         mark_included( _head_block_id, false );

         // update the block_num_to_block_id index
         _block_num_to_id_db.remove( _head_block_header.block_num );

         auto previous_block_id = _head_block_header.previous;

         // fetch the undo state for the head block
         auto undo_state = _undo_state_db.fetch( _head_block_id );
         undo_state.set_prev_state( self->shared_from_this() );
         undo_state.apply_changes();

         _head_block_id = previous_block_id;
         _head_block_header = self->get_block_header( _head_block_id );

         if( _observer ) _observer->state_changed(undo_state.shared_from_this());

      } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   } // namespace detail

   chain_database::chain_database()
   :my( new detail::chain_database_impl() )
   {
      my->self = this;
   }

   chain_database::~chain_database()
   {
      try {
         close();
      }
      catch ( const fc::exception& e )
      {
         wlog( "unexpected exception closing database\n ${e}", ("e",e.to_detail_string() ) );
      }
      catch ( ... )
      {
         wlog( "unexpected exception closing database\n" );
      }
   }
   std::vector<account_id_type> chain_database::next_round_active_delegates()const
   {
      return get_delegates_by_vote( 0, BTS_BLOCKCHAIN_NUM_DELEGATES );
   }

   /**
    *  @return the top BTS_BLOCKCHAIN_NUM_DELEGATES by vote
    */
   std::vector<account_id_type> chain_database::get_delegates_by_vote(uint32_t first, uint32_t count )const
   { try {
      auto del_vote_itr = my->_delegate_vote_index_db.begin();
      std::vector<account_id_type> sorted_delegates;
      uint32_t pos = 0;
      while( sorted_delegates.size() < count && del_vote_itr.valid() )
      {
         if( pos >= first )
            sorted_delegates.push_back( del_vote_itr.key().delegate_id );
         ++pos;
         ++del_vote_itr;
      }
      return sorted_delegates;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   /**
    *  @return the top BTS_BLOCKCHAIN_NUM_DELEGATES by vote
    */
   std::vector<account_record> chain_database::get_delegate_records_by_vote(uint32_t first, uint32_t count )const
   { try {
      auto del_vote_itr = my->_delegate_vote_index_db.begin();
      std::vector<account_record> sorted_delegates;
      uint32_t pos = 0;
      while( sorted_delegates.size() < count && del_vote_itr.valid() )
      {
         if( pos >= first )
            sorted_delegates.push_back( *get_account_record(del_vote_itr.value()) );
         ++pos;
         ++del_vote_itr;
      }
      return sorted_delegates;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void chain_database::open( const fc::path& data_dir, fc::path genesis_file )
   { try {
      bool is_new_data_dir = !fc::exists( data_dir );
      try
      {
          fc::create_directories( data_dir );

          my->_fork_number_db.open( data_dir / "fork_number_db" );
          my->_fork_db.open( data_dir / "fork_db" );
          my->_property_db.open( data_dir / "property_db" );
          my->_proposal_db.open( data_dir / "proposal_db" );
          my->_proposal_vote_db.open( data_dir / "proposal_vote_db" );

          my->_undo_state_db.open( data_dir / "undo_state_db" );

          my->_block_num_to_id_db.open( data_dir / "block_num_to_id_db" );
          my->_block_id_to_block_db.open( data_dir / "block_id_to_block_db" );

          my->_pending_transaction_db.open( data_dir / "pending_transaction_db" );

          my->_asset_db.open( data_dir / "asset_db" );
          my->_balance_db.open( data_dir / "balance_db" );
          my->_account_db.open( data_dir / "account_db" );
          my->_address_to_account_db.open( data_dir / "address_to_account_db" );

          my->_account_index_db.open( data_dir / "account_index_db" );
          my->_symbol_index_db.open( data_dir / "symbol_index_db" );
          my->_delegate_vote_index_db.open( data_dir / "delegate_vote_index_db" );

          my->_ask_db.open( data_dir / "ask_db" );
          my->_bid_db.open( data_dir / "bid_db" );
          my->_short_db.open( data_dir / "short_db" );
          my->_collateral_db.open( data_dir / "collateral_db" );

          my->_processed_transaction_id_db.open( data_dir / "processed_transaction_id_db" );

          // TODO: check to see if we crashed during the last write
          //   if so, then apply the last undo operation stored.

          uint32_t       last_block_num = -1;
          block_id_type  last_block_id;
          my->_block_num_to_id_db.last( last_block_num, last_block_id );
          if( last_block_num != uint32_t(-1) )
          {
             my->_head_block_header = get_block( last_block_id );
             my->_head_block_id = last_block_id;
          }

          //  process the pending transactions to cache by fees
          auto pending_itr = my->_pending_transaction_db.begin();
          while( pending_itr.valid() )
          {
             try {
                auto trx = pending_itr.value();
                auto trx_id = trx.id();
                auto eval_state = evaluate_transaction( trx );
                share_type fees = eval_state->get_fees();
                my->_pending_fee_index[ fee_index( fees, trx_id ) ] = eval_state;
                my->_pending_transaction_db.store( trx_id, trx );
             }
             catch ( const fc::exception& e )
             {
                wlog( "error processing pending transaction: ${e}", ("e",e.to_detail_string() ) );
             }
             ++pending_itr;
          }

          if( last_block_num == uint32_t(-1) )
             my->initialize_genesis(genesis_file);
          my->_chain_id = get_property( bts::blockchain::chain_id ).as<digest_type>();
      }
      catch( ... )
      {
          close();
          if( is_new_data_dir ) fc::remove_all( data_dir );
          throw;
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("data_dir",data_dir) ) }

   void chain_database::close()
   { try {
      my->_fork_number_db.close();
      my->_fork_db.close();
      my->_property_db.close();
      my->_proposal_db.close();
      my->_proposal_vote_db.close();

      my->_undo_state_db.close();

      my->_block_num_to_id_db.close();
      my->_block_id_to_block_db.close();

      my->_pending_transaction_db.close();

      my->_asset_db.close();
      my->_balance_db.close();
      my->_account_db.close();
      my->_address_to_account_db.close();

      my->_account_index_db.close();
      my->_symbol_index_db.close();
      my->_delegate_vote_index_db.close();

      my->_ask_db.close();
      my->_bid_db.close();
      my->_short_db.close();
      my->_collateral_db.close();

      my->_processed_transaction_id_db.close();
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   account_id_type chain_database::get_signing_delegate_id( fc::time_point_sec sec )const
   { try {
      FC_ASSERT( sec + 3600 >= my->_head_block_header.timestamp, 
                 "local clock is over 1 hour behind most recent delegate", ("sec",sec) );

      uint64_t  interval_number = sec.sec_since_epoch() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      uint32_t  delegate_pos = (uint32_t)(interval_number % BTS_BLOCKCHAIN_NUM_DELEGATES);
      auto sorted_delegates = get_active_delegates();

      FC_ASSERT( delegate_pos < sorted_delegates.size() );
      return  sorted_delegates[delegate_pos];
   } FC_RETHROW_EXCEPTIONS( warn, "", ("sec",sec) ) }

   public_key_type chain_database::get_signing_delegate_key( fc::time_point_sec sec )const
   { try {
      auto delegate_record = get_account_record( get_signing_delegate_id( sec ) );
      FC_ASSERT( !!delegate_record );
      return delegate_record->active_key();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("sec", sec) ) }

   transaction_evaluation_state_ptr chain_database::evaluate_transaction( const signed_transaction& trx )
   { try {
      pending_chain_state_ptr          pend_state = std::make_shared<pending_chain_state>(shared_from_this());
      transaction_evaluation_state_ptr trx_eval_state = std::make_shared<transaction_evaluation_state>(pend_state,my->_chain_id);

      trx_eval_state->evaluate( trx );

      return trx_eval_state;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("trx",trx) ) }

   signed_block_header  chain_database::get_block_header( const block_id_type& block_id )const
   { try {
      return get_block( block_id );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id) ) }

   signed_block_header  chain_database::get_block_header( uint32_t block_num )const
   { try {
      return get_block( block_num );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block_num",block_num) ) }

   full_block           chain_database::get_block( const block_id_type& block_id )const
   { try {
      return my->_block_id_to_block_db.fetch(block_id);
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id) ) }

   full_block           chain_database::get_block( uint32_t block_num )const
   { try {
      auto block_id = my->_block_num_to_id_db.fetch( block_num );
      return get_block( block_id );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block_num",block_num) ) }

   signed_block_header  chain_database::get_head_block()const
   {
      return my->_head_block_header;
   }

   otransaction_location chain_database::get_transaction_location( const transaction_id_type& trx_id )const
   {
      return my->_processed_transaction_id_db.fetch_optional( trx_id );
   }

   /**
    *  Adds the block to the database and manages any reorganizations as a result.
    *
    */
   void chain_database::push_block( const full_block& block_data )
   { try {
      auto block_id        = block_data.id();
      auto current_head_id = my->_head_block_id;

      block_fork_data fork = my->store_and_index( block_id, block_data );

      //ilog( "previous ${p} ==? current ${c}", ("p",block_data.previous)("c",current_head_id) );
      if( block_data.previous == current_head_id )
      {
         // attempt to extend chain
         return my->extend_chain( block_data );
      }
      else if( fork.can_link() && block_data.block_num > my->_head_block_header.block_num )
      {
         try {
            my->switch_to_fork( block_id );
         }
         catch ( const fc::exception& e )
         {
            wlog( "attempt to switch to fork failed: ${e}, reverting", ("e",e.to_detail_string() ) );
            my->switch_to_fork( current_head_id );
         }
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block",block_data) ) }




   /** return the timestamp from the head block */
   fc::time_point_sec   chain_database::now()const
   {
      return my->_head_block_header.timestamp;
   }

         /** return the current fee rate in millishares */
   int64_t              chain_database::get_fee_rate()const
   {
      return my->_head_block_header.fee_rate;
   }

   int64_t              chain_database::get_delegate_pay_rate()const
   {
      return my->_head_block_header.delegate_pay_rate;
   }


   oasset_record        chain_database::get_asset_record( asset_id_type id )const
   {
      auto itr = my->_asset_db.find( id );
      if( itr.valid() )
      {
         return itr.value(); 
      }
      return oasset_record();
   }
   oaccount_record      chain_database::get_account_record( const address& owner )const
   { try {
      auto itr = my->_address_to_account_db.find( owner );
      if( itr.valid() )
      {
         return get_account_record( itr.value() );
      }
      return oaccount_record();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("owner",owner) ) }

   obalance_record      chain_database::get_balance_record( const balance_id_type& balance_id )const
   {
      return my->_balance_db.fetch_optional( balance_id );
   }

   oaccount_record         chain_database::get_account_record( account_id_type account_id )const
   {
      return my->_account_db.fetch_optional( account_id );
   }

   asset_id_type        chain_database::get_asset_id( const string& symbol )const
   { try {
      auto arec = get_asset_record( symbol );
      FC_ASSERT( arec.valid() );
      return arec->id;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol) ) }

   bool                 chain_database::is_valid_symbol( const string& symbol )const
   {
      return get_asset_record(symbol).valid();
   }
   
   oasset_record        chain_database::get_asset_record( const string& symbol )const
   { try {
       auto symbol_id_itr = my->_symbol_index_db.find( symbol );
       if( symbol_id_itr.valid() )
       {
          return get_asset_record( symbol_id_itr.value() );
       }
       else
          wlog( "    unable to find '${symbol}'", ("symbol",symbol) );
       return oasset_record();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol) ) }

   oaccount_record         chain_database::get_account_record( const string& name )const
   { try {
       auto account_id_itr = my->_account_index_db.find( name );
       if( account_id_itr.valid() )
          return get_account_record( account_id_itr.value() );
       return oaccount_record();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name) ) }


   void chain_database::store_asset_record( const asset_record& r )
   { try {
       if( r.is_null() )
       {
          my->_asset_db.remove( r.id );
          my->_symbol_index_db.remove( r.symbol );
       }
       else
       {
          my->_asset_db.store( r.id, r );
          my->_symbol_index_db.store( r.symbol, r.id );
       }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("record", r) ) }


   void chain_database::store_balance_record( const balance_record& r )
   { try {
       if( r.is_null() )
       {
          my->_balance_db.remove( r.id() );
       }
       else
       {
          my->_balance_db.store( r.id(), r );
       }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("record", r) ) }


   void chain_database::store_account_record( const account_record& record_to_store )
   { try {
       oaccount_record old_rec = get_account_record( record_to_store.id );

       if( record_to_store.is_null() && old_rec)
       {
          my->_account_db.remove( record_to_store.id );
          my->_account_index_db.remove( record_to_store.name );

          for( auto item : old_rec->active_key_history )
             my->_address_to_account_db.remove( address(item.second) );

          if( old_rec->is_delegate() )
          {
              auto itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
              my->_delegate_vote_index_db.remove( vote_del( old_rec->net_votes(), 
                                                            record_to_store.id ) );
              itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
          }
       }
       else if( !record_to_store.is_null() )
       {
          my->_account_db.store( record_to_store.id, record_to_store );
          my->_account_index_db.store( record_to_store.name, record_to_store.id );

          for( auto item : record_to_store.active_key_history )
          { // re-index all keys for this record
             my->_address_to_account_db.store( address(item.second), record_to_store.id );
          }


          if( old_rec.valid() && old_rec->is_delegate() )
          {
              auto itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
              my->_delegate_vote_index_db.remove( vote_del( old_rec->net_votes(), 
                                                            record_to_store.id ) );
              itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
          }


          if( record_to_store.is_delegate() )
          {
              auto itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
              my->_delegate_vote_index_db.store( vote_del( record_to_store.net_votes(),
                                                           record_to_store.id ), 
                                                0/*dummy value*/ );
              itr = my->_delegate_vote_index_db.begin();
              while( itr.valid() )
              {
                  ++itr;
              }
          }
       }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("record", record_to_store) ) }

   void  chain_database::store_transaction_location( const transaction_id_type& trx_id,
                                                     const transaction_location& loc )
   { try {
       my->_processed_transaction_id_db.store( trx_id, loc );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("id", trx_id)("location",loc) ) }



   osigned_transaction chain_database::get_transaction( const transaction_id_type& trx_id )const
   { try {

      auto trx_loc = get_transaction_location( trx_id );
      ilog( "block_number: ${trx_loc}", ("trx_loc",trx_loc) );
      if( !trx_loc ) return osigned_transaction();
      auto block_id = my->_block_num_to_id_db.fetch( trx_loc->block_num );
      auto block_data = my->_block_id_to_block_db.fetch( block_id );
      FC_ASSERT( block_data.user_transactions.size() > trx_loc->trx_num );

      return block_data.user_transactions[ trx_loc->trx_num ];
   } FC_RETHROW_EXCEPTIONS( warn, "", ("trx_id",trx_id) ) }

   void    chain_database::scan_assets( function<void( const asset_record& )> callback )
   {
        auto asset_itr = my->_asset_db.begin();
        while( asset_itr.valid() )
        {
           callback( asset_itr.value() );
           ++asset_itr;
        }
   }

   void    chain_database::scan_balances( function<void( const balance_record& )> callback )
   {
        auto balances = my->_balance_db.begin();
        while( balances.valid() )
        {
           callback( balances.value() );
           ++balances;
        }
   }
   void    chain_database::scan_accounts( function<void( const account_record& )> callback )
   {
        auto name_itr = my->_account_db.begin();
        while( name_itr.valid() )
        {
           callback( name_itr.value() );
           ++name_itr;
        }
   }

   /** this should throw if the trx is invalid */
   transaction_evaluation_state_ptr chain_database::store_pending_transaction( const signed_transaction& trx )
   { try {
      auto trx_id = trx.id();
      auto current_itr = my->_pending_transaction_db.find( trx_id );
      if( current_itr.valid() ) return nullptr;

      auto eval_state = evaluate_transaction( trx );
      share_type fees = eval_state->get_fees();
      my->_pending_fee_index[ fee_index( fees, trx_id ) ] = eval_state;
      my->_pending_transaction_db.store( trx_id, trx );

      return eval_state;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("trx",trx) ) }

   /** returns all transactions that are valid (indepdnent of eachother) sorted by fee */
   std::vector<transaction_evaluation_state_ptr> chain_database::get_pending_transactions()const
   {
      std::vector<transaction_evaluation_state_ptr> trxs;
      for( auto item : my->_pending_fee_index )
      {
          trxs.push_back( item.second );
      }
      return trxs;
   }
   bool chain_database::is_known_transaction( const transaction_id_type& trx_id )
   {
      auto pending_itr = my->_pending_transaction_db.find( trx_id );
      if( pending_itr.valid() ) return true;
      return !!get_transaction_location( trx_id );
   }

   full_block chain_database::generate_block( fc::time_point_sec timestamp )
   {
      full_block next_block;

      pending_chain_state_ptr pending_state = std::make_shared<pending_chain_state>(shared_from_this());
      auto pending_trx = get_pending_transactions();

      size_t block_size = 0;
      share_type total_fees = 0;
      for( auto item : pending_trx )
      {
         auto trx_size = item->trx.data_size();
         if( block_size + trx_size > BTS_BLOCKCHAIN_MAX_BLOCK_SIZE )
            break;
         block_size += trx_size;
         // make modifications to tempoary state...
         pending_chain_state_ptr pending_trx_state = std::make_shared<pending_chain_state>(pending_state);
         transaction_evaluation_state_ptr trx_eval_state = 
             std::make_shared<transaction_evaluation_state>(pending_trx_state,my->_chain_id);

         try {
            trx_eval_state->evaluate( item->trx );
            // TODO: what about fees in other currencies?
            total_fees += trx_eval_state->get_fees(0);
            // apply temporary state to block state
            pending_trx_state->apply_changes();
            next_block.user_transactions.push_back( item->trx );
         }
         catch ( const fc::exception& e )
         {
            wlog( "pending transaction was found to be invalid in context of block\n ${trx} \n${e}",
                  ("trx",fc::json::to_pretty_string(item->trx) )("e",e.to_detail_string()) );
         }
      }

      next_block.block_num          = my->_head_block_header.block_num + 1;
      next_block.previous           = my->_head_block_id;
      next_block.timestamp          = timestamp;
      next_block.fee_rate           = next_block.next_fee( my->_head_block_header.fee_rate, block_size );
      next_block.transaction_digest = digest_block(next_block).calculate_transaction_digest();

      // TODO: adjust fees vs dividends here...  right now 100% of fees are paid to delegates
      next_block.delegate_pay_rate  = next_block.next_delegate_pay( my->_head_block_header.delegate_pay_rate, total_fees );


    //  elog( "initial pay rate: ${R}   total fees: ${F} next: ${N}",
    //        ( "R", my->_head_block_header.delegate_pay_rate )( "F", total_fees )("N",next_block.delegate_pay_rate) );

      return next_block;
   }

   void detail::chain_database_impl::initialize_genesis(fc::path genesis_file)
   { try {
      if( self->chain_id() != digest_type() )
      {
         self->sanity_check();
         ilog( "Genesis state already initialized" );
         return;
      }

      std::cout << "Initializing genesis state\n";
      FC_ASSERT( fc::exists( genesis_file ), "Genesis file '${file}' was not found.", ("file",genesis_file) );

      genesis_block_config config;
      if( genesis_file.extension() == ".json" )
      {
         config = fc::json::from_file(genesis_file).as<genesis_block_config>();
      }
      else if( genesis_file.extension() == ".dat" )
      {
         fc::ifstream in( genesis_file );
         fc::raw::unpack( in, config );
      }
      else
      {
         FC_ASSERT( !"Invalid genesis format", " '${format}'", ("format",genesis_file.extension() ) );
      }

      fc::sha256::encoder enc;
      fc::raw::pack( enc, config );
      _chain_id = enc.result();
      self->set_property( bts::blockchain::chain_id, fc::variant(_chain_id) );

      fc::uint128 total_unscaled = 0;
      for( auto item : config.balances ) total_unscaled += int64_t(item.second/1000);
      ilog( "total unscaled: ${s}", ("s", total_unscaled) );

      std::vector<name_config> delegate_config;
      for( auto item : config.names )
         if( item.is_delegate ) delegate_config.push_back( item );

      FC_ASSERT( delegate_config.size() >= BTS_BLOCKCHAIN_NUM_DELEGATES,
                 "genesis.json does not contain enough initial delegates",
                 ("required",BTS_BLOCKCHAIN_NUM_DELEGATES)("provided",delegate_config.size()) );

      account_record god; god.id = 0; god.name = "god";
      self->store_account_record( god );


      fc::time_point_sec timestamp = config.timestamp;
      std::vector<account_id_type> delegate_ids;
      int32_t account_id = 1;
      for( auto name : config.names )
      {
         account_record rec;
         rec.id                = account_id;
         rec.name              = name.name;
         rec.owner_key         = name.owner;
         rec.set_active_key( fc::time_point_sec(),  name.owner );
         rec.registration_date = timestamp;
         rec.last_update       = timestamp;
         if( name.is_delegate )
         {
            rec.delegate_info = delegate_stats();
            delegate_ids.push_back( account_id );
         }
         self->store_account_record( rec );
         ++account_id;
      }

      for( auto item : config.balances )
      {
         for( auto delegate_id : delegate_ids )
         {
            fc::uint128 initial( int64_t(item.second/1000) );
            initial *= fc::uint128(int64_t(BTS_BLOCKCHAIN_INITIAL_SHARES));
            initial /= total_unscaled;
            initial /= int64_t(delegate_ids.size());
            balance_record initial_balance( item.first,
                                            asset( share_type( initial.low_bits() ), 0 ),
                                            delegate_id );
            // in case of redundant balances
            auto cur = self->get_balance_record( initial_balance.id() );
            if( cur.valid() ) initial_balance.balance += cur->balance;
            self->store_balance_record( initial_balance );
            auto da = _account_db.fetch( delegate_id );
            da.delegate_info->votes_for += initial.low_bits();
            self->store_account_record( da );
         //   _account_db.store( da.id, da );
         }
      }

      asset total;
      auto itr = _balance_db.begin();
      while( itr.valid() )
      {
         auto ind = itr.value().get_balance();
         FC_ASSERT( ind.amount >= 0, "", ("record",itr.value()) );
         total += ind;
         ++itr;
      }

      asset_record base_asset;
      base_asset.id = 0;
      base_asset.symbol = BTS_ADDRESS_PREFIX;
      base_asset.name = "BitShares XTS";
      base_asset.description = "Shares in the DAC";
      base_asset.issuer_account_id = god.id;
      base_asset.current_share_supply = total.amount;
      base_asset.maximum_share_supply = BTS_BLOCKCHAIN_INITIAL_SHARES;
      base_asset.collected_fees = 0;
      self->store_asset_record( base_asset );

      block_fork_data gen_fork;
      gen_fork.is_valid = true;
      gen_fork.is_included = true;
      gen_fork.is_linked = true;
      _fork_db.store( block_id_type(), gen_fork );

      self->set_property( chain_property_enum::active_delegate_list_id, fc::variant(self->next_round_active_delegates()) );
      self->set_property( chain_property_enum::last_asset_id, 0 );
      self->set_property( chain_property_enum::last_proposal_id, 0 );
      self->set_property( chain_property_enum::last_account_id, uint64_t(config.names.size()) );
      self->set_property( chain_property_enum::last_random_seed_id, fc::variant(secret_hash_type()) );

      self->sanity_check();
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void chain_database::set_observer( chain_observer* observer )
   {
      my->_observer = observer;
   }
   bool chain_database::is_known_block( const block_id_type& block_id )const
   {
      auto itr = my->_block_id_to_block_db.find( block_id );
      return itr.valid();
   }
   uint32_t chain_database::get_block_num( const block_id_type& block_id )const
   { try {
      if( block_id == block_id_type() )
         return 0;
      return my->_block_id_to_block_db.fetch( block_id ).block_num;
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to find block ${block_id}", ("block_id", block_id) ) }

    uint32_t         chain_database::get_head_block_num()const
    {
       return my->_head_block_header.block_num;
    }

    block_id_type      chain_database::get_head_block_id()const
    {
       return my->_head_block_id;
    }
    std::vector<account_record> chain_database::get_accounts( const string& first, uint32_t count )const
    { try {
       auto itr = my->_account_index_db.lower_bound(first);
       std::vector<account_record> names;
       while( itr.valid() && names.size() < count )
       {
          names.push_back( *get_account_record( itr.value() ) );
          ++itr;
       }
       return names;
    } FC_RETHROW_EXCEPTIONS( warn, "", ("first",first)("count",count) )  }


    std::vector<asset_record> chain_database::get_assets( const string& first_symbol, uint32_t count )const
    { try {
       auto itr = my->_symbol_index_db.lower_bound(first_symbol);
       std::vector<asset_record> assets;
       while( itr.valid() && assets.size() < count )
       {
          assets.push_back( *get_asset_record( itr.value() ) );
          ++itr;
       }
       return assets;
    } FC_RETHROW_EXCEPTIONS( warn, "", ("first_symbol",first_symbol)("count",count) )  }

    void chain_database::export_fork_graph( const fc::path& filename )const
    {
       std::ofstream out( filename.generic_string().c_str() );
       out << "digraph G { \n"; 
       out << "rankdir=RL;\n";
          auto fork_itr = my->_fork_db.begin();
          while( fork_itr.valid() )
          {
             auto fork_data = fork_itr.value();
             ilog( "${id} => ${r}", ("id",fork_itr.key())("r",fork_data) );
             for( auto next : fork_data.next_blocks )
             {
                out << '"' << string ( fork_itr.key() ).substr(0,5) <<"\" "
                    << "[color=" << (fork_data.is_included ? "green" : "lightblue") << ",style=filled,"
                    << " shape=" << (fork_data.is_linked  ? "ellipse" : "box" ) << "];\n";
                out << '"' << string ( next ).substr(0,5) <<"\" -> \"" << string( fork_itr.key() ).substr(0,5) << "\";\n";
            }
             ++fork_itr;
          }
       out << "}"; 
    }

   fc::variant chain_database::get_property( chain_property_enum property_id )const
   { try {
      return my->_property_db.fetch( property_id );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("property_id",property_id) ) }

   void  chain_database::set_property( chain_property_enum property_id, 
                                                     const fc::variant& property_value )
   {
      if( property_value.is_null() )
         my->_property_db.remove( property_id );
      else
         my->_property_db.store( property_id, property_value );
   }
   void chain_database::store_proposal_record( const proposal_record& r )
   {
      if( r.is_null() )
      {
         my->_proposal_db.remove( r.id );
      }
      else
      {
         my->_proposal_db.store( r.id, r );
      }
   }

   oproposal_record chain_database::get_proposal_record( proposal_id_type id )const
   {
      return my->_proposal_db.fetch_optional(id);
   }
                                                                                            
   void chain_database::store_proposal_vote( const proposal_vote& r )
   {
      if( r.is_null() )
      {
         my->_proposal_vote_db.remove( r.id );
      }
      else
      {
         my->_proposal_vote_db.store( r.id, r );
      }
   }

   oproposal_vote chain_database::get_proposal_vote( proposal_vote_id_type id )const
   {
      return my->_proposal_vote_db.fetch_optional(id);
   }

   digest_type chain_database::chain_id()const
   {
         return my->_chain_id;
   }
   std::vector<proposal_record>  chain_database::get_proposals( uint32_t first, uint32_t count )const
   {
      std::vector<proposal_record> results;
      auto current_itr = my->_proposal_db.lower_bound( first );
      uint32_t found = 0;
      while( current_itr.valid() && found < count )
      {
         results.push_back( current_itr.value() );
         ++found;
         ++current_itr;
      }
      return results;
   }
   std::vector<proposal_vote>   chain_database::get_proposal_votes( proposal_id_type proposal_id ) const
   {
      std::vector<proposal_vote> results;
      auto current_itr = my->_proposal_vote_db.lower_bound( proposal_vote_id_type(proposal_id,0) );
      while( current_itr.valid() )
      {
         if( current_itr.key().proposal_id != proposal_id )
            return results;

         results.push_back( current_itr.value() );
         ++current_itr;
      }
      return results;
   }

   fc::ripemd160    chain_database::get_current_random_seed()const
   {
      return get_property( last_random_seed_id ).as<fc::ripemd160>();
   }

   oorder_record         chain_database::get_bid_record( const market_index_key&  key )const
   {
      return my->_bid_db.fetch_optional(key);
   }
   oorder_record         chain_database::get_ask_record( const market_index_key&  key )const
   {
      return my->_ask_db.fetch_optional(key);
   }
   oorder_record         chain_database::get_short_record( const market_index_key& key )const
   {
      return my->_short_db.fetch_optional(key);
   }
   ocollateral_record    chain_database::get_collateral_record( const market_index_key& key )const
   {
      return my->_collateral_db.fetch_optional(key);
   }
                                                                                              
   void chain_database::store_bid_record( const market_index_key& key, const order_record& order ) 
   {
      if( order.is_null() )
         my->_bid_db.remove( key );
      else
         my->_bid_db.store( key, order );
   }
   void chain_database::store_ask_record( const market_index_key& key, const order_record& order ) 
   {
      if( order.is_null() )
         my->_ask_db.remove( key );
      else
         my->_ask_db.store( key, order );
   }
   void chain_database::store_short_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_short_db.remove( key );
      else
         my->_short_db.store( key, order );
   }
   void chain_database::store_collateral_record( const market_index_key& key, const collateral_record& collateral ) 
   {
      if( collateral.is_null() )
         my->_collateral_db.remove( key );
      else
         my->_collateral_db.store( key, collateral );
   }
   string  chain_database::get_asset_symbol( asset_id_type asset_id )const
   { try {
      auto asset_rec = get_asset_record( asset_id );
      FC_ASSERT( asset_rec.valid(), "Unknown Asset ID: ${id}", ("asset_id",asset_id) );
      return asset_rec->symbol;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("asset_id",asset_id) ) }

   void chain_database::sanity_check()const
   { try {
      asset total;
      auto itr = my->_balance_db.begin();
      while( itr.valid() )
      {
         auto ind = itr.value().get_balance();
         if( ind.asset_id == 0 )
         {
            FC_ASSERT( ind.amount >= 0, "", ("record",itr.value()) );
            total += ind;
         }
         ++itr;
      }
      int64_t total_votes = 0;
      auto aitr = my->_account_db.begin();
      while( aitr.valid() )
      {
         auto v = aitr.value();
         if( v.is_delegate() )
         {
            total += asset(v.delegate_info->pay_balance);
            total_votes += v.delegate_info->votes_for + v.delegate_info->votes_against;
         }
         ++aitr;
      }

      FC_ASSERT( total_votes == total.amount, "", 
                 ("total_votes",total_votes)
                 ("total_shares",total) );
     
      auto ar = get_asset_record( asset_id_type(0) );
      FC_ASSERT( ar.valid() );
      FC_ASSERT( ar->current_share_supply == total.amount, "", ("ar",ar)("total",total)("delta",ar->current_share_supply-total.amount) );
      FC_ASSERT( ar->current_share_supply <= ar->maximum_share_supply );
      //std::cerr << "Total Balances: " << to_pretty_asset( total ) << "\n";
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

} } // namespace bts::blockchain
