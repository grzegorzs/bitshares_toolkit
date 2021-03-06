#pragma once
#include <bts/blockchain/types.hpp>
#include <bts/blockchain/chain_interface.hpp>
#include <bts/blockchain/pending_chain_state.hpp>
#include <bts/blockchain/block.hpp>

#include <fc/filesystem.hpp>

#include <functional>

namespace bts { namespace blockchain {


   namespace detail { class chain_database_impl; }

   struct block_summary
   {
      full_block                                    block_data;
      pending_chain_state_ptr                       applied_changes;
   };

   class chain_observer
   {
      public:
         virtual ~chain_observer(){}

         /**
          * This method is called anytime the blockchain state changes including
          * undo operations.
          */
         virtual void state_changed( const pending_chain_state_ptr& state ) = 0;
         /**
          *  This method is called anytime a block is applied to the chain.
          */
         virtual void block_applied( const block_summary& summary ) = 0;
   };

   class chain_database : public chain_interface, public std::enable_shared_from_this<chain_database>
   {
      public:
         chain_database();
         virtual ~chain_database()override;

         void open( const fc::path& data_dir, fc::path genesis_file );
         void close();

         void set_observer( chain_observer* observer );
         void sanity_check()const;

         transaction_evaluation_state_ptr         store_pending_transaction( const signed_transaction& trx );
         vector<transaction_evaluation_state_ptr> get_pending_transactions()const;
         bool                                     is_known_transaction( const transaction_id_type& trx_id );
         void                                     export_fork_graph( const fc::path& filename )const;

         /** Produce a block for the given timeslot, the block is not signed because that is the
          *  role of the wallet.
          */
         full_block                    generate_block( time_point_sec timestamp );

         /**
          *  The chain ID is the hash of the initial_config loaded when the
          *  database was first created.
          */
         digest_type                   chain_id()const;

         bool                          is_known_block( const block_id_type& block_id )const;

         fc::ripemd160                 get_current_random_seed()const override;
         public_key_type               get_signing_delegate_key( time_point_sec )const;
         account_id_type               get_signing_delegate_id( time_point_sec )const;
         uint32_t                      get_block_num( const block_id_type& )const;
         signed_block_header           get_block_header( const block_id_type& )const;
         signed_block_header           get_block_header( uint32_t block_num )const;
         full_block                    get_block( const block_id_type& )const;
         full_block                    get_block( uint32_t block_num )const;
         signed_block_header           get_head_block()const;
         uint32_t                      get_head_block_num()const;
         block_id_type                 get_head_block_id()const;
         osigned_transaction           get_transaction( const transaction_id_type& trx_id )const;
         virtual otransaction_location get_transaction_location( const transaction_id_type& trx_id )const override;

         vector<account_record >       get_accounts( const string& first, uint32_t count )const;
         vector<asset_record>          get_assets( const string& first_symbol, uint32_t count )const;

         /** should perform any chain reorganization required
          *
          *  @return the pending chain state generated as a result of pushing the block,
          *  this state can be used by wallets to scan for changes without the wallets
          *  having to process raw transactions.
          **/
         virtual void push_block( const full_block& block_data );


         /**
          *  Evaluate the transaction and return the results.
          */
         virtual transaction_evaluation_state_ptr evaluate_transaction( const signed_transaction& trx );


         /** return the timestamp from the head block */
         virtual time_point_sec         now()const override;

         /** return the current fee rate in millishares */
         virtual int64_t                    get_fee_rate()const override;
         virtual int64_t                    get_delegate_pay_rate()const override;

         /** top delegates by current vote, projected to be active in the next round */
         vector<account_id_type>            next_round_active_delegates()const;
                                            
         vector<account_id_type>            get_delegates_by_vote( uint32_t first=0, uint32_t count = uint32_t(-1) )const;
         vector<account_record>             get_delegate_records_by_vote( uint32_t first=0, uint32_t count = uint32_t(-1))const;
         vector<proposal_record>            get_proposals( uint32_t first=0, uint32_t count = uint32_t(-1))const;
         vector<proposal_vote>              get_proposal_votes( proposal_id_type proposal_id ) const;

         void                               scan_assets( function<void( const asset_record& )> callback );
         void                               scan_balances( function<void( const balance_record& )> callback );
         void                               scan_accounts( function<void( const account_record& )> callback );

         virtual variant                    get_property( chain_property_enum property_id )const override;
         virtual void                       set_property( chain_property_enum property_id, 
                                                          const variant& property_value )override;

         bool                               is_valid_symbol( const string& asset_symbol )const;
         string                             get_asset_symbol( asset_id_type asset_id )const;
         asset_id_type                      get_asset_id( const string& asset_sybmol )const;
         virtual oasset_record              get_asset_record( asset_id_type id )const override;
         virtual obalance_record            get_balance_record( const balance_id_type& id )const override;
         virtual oaccount_record            get_account_record( account_id_type id )const override;
         virtual oaccount_record            get_account_record( const address& owner )const override;

         virtual oasset_record              get_asset_record( const string& symbol )const override;
         virtual oaccount_record            get_account_record( const string& name )const override;

         virtual void                       store_asset_record( const asset_record& r )override;
         virtual void                       store_balance_record( const balance_record& r )override;
         virtual void                       store_account_record( const account_record& r )override;
         virtual void                       store_transaction_location( const transaction_id_type&,
                                                                  const transaction_location& loc )override;

         virtual void                       store_proposal_record( const proposal_record& r )override;
         virtual oproposal_record           get_proposal_record( proposal_id_type id )const override;
                                                                                                          
         virtual void                       store_proposal_vote( const proposal_vote& r )override;
         virtual oproposal_vote             get_proposal_vote( proposal_vote_id_type id )const override;

         virtual oorder_record              get_bid_record( const market_index_key& )const override;
         virtual oorder_record              get_ask_record( const market_index_key& )const override;
         virtual oorder_record              get_short_record( const market_index_key& )const override;
         virtual ocollateral_record         get_collateral_record( const market_index_key& )const override;
                                                                                                            
         virtual void                       store_bid_record( const market_index_key& key, const order_record& ) override;
         virtual void                       store_ask_record( const market_index_key& key, const order_record& ) override;
         virtual void                       store_short_record( const market_index_key& key, const order_record& ) override;
         virtual void                       store_collateral_record( const market_index_key& key, const collateral_record& ) override;

      private:
         unique_ptr<detail::chain_database_impl> my;
   };

   typedef shared_ptr<chain_database> chain_database_ptr;

} } // bts::blockchain

