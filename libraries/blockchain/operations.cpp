#include <bts/blockchain/operations.hpp>
#include <bts/blockchain/fire_operation.hpp>
#include <bts/blockchain/operation_factory.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/io/raw_variant.hpp>

namespace bts { namespace blockchain {
   const operation_type_enum withdraw_operation::type          = withdraw_op_type;
   const operation_type_enum deposit_operation::type           = deposit_op_type;
   const operation_type_enum create_asset_operation::type      = create_asset_op_type;
   const operation_type_enum update_asset_operation::type      = update_asset_op_type;
   const operation_type_enum issue_asset_operation::type       = issue_asset_op_type;
   const operation_type_enum register_account_operation::type  = register_account_op_type;
   const operation_type_enum update_account_operation::type    = update_account_op_type;
   const operation_type_enum withdraw_pay_operation::type      = withdraw_pay_op_type;
   const operation_type_enum submit_proposal_operation::type   = submit_proposal_op_type;
   const operation_type_enum vote_proposal_operation::type     = vote_proposal_op_type;
   const operation_type_enum bid_operation::type               = bid_op_type;
   const operation_type_enum ask_operation::type               = ask_op_type;
   const operation_type_enum short_operation::type             = short_op_type;
   const operation_type_enum cover_operation::type             = cover_op_type;
   const operation_type_enum add_collateral_operation::type    = add_collateral_op_type;
   const operation_type_enum remove_collateral_operation::type = remove_collateral_op_type;

   static bool first_chain = []()->bool{
      bts::blockchain::operation_factory::instance().register_operation<withdraw_operation>();
      bts::blockchain::operation_factory::instance().register_operation<deposit_operation>();
      bts::blockchain::operation_factory::instance().register_operation<create_asset_operation>();
      bts::blockchain::operation_factory::instance().register_operation<issue_asset_operation>();
      bts::blockchain::operation_factory::instance().register_operation<update_asset_operation>();
      bts::blockchain::operation_factory::instance().register_operation<register_account_operation>();
      bts::blockchain::operation_factory::instance().register_operation<withdraw_pay_operation>();
      bts::blockchain::operation_factory::instance().register_operation<update_account_operation>();
      bts::blockchain::operation_factory::instance().register_operation<fire_delegate_operation>();
      bts::blockchain::operation_factory::instance().register_operation<submit_proposal_operation>();
      bts::blockchain::operation_factory::instance().register_operation<vote_proposal_operation>();
      return true;
   }();

   balance_id_type  deposit_operation::balance_id()const
   {
      return condition.get_address();
   }

   deposit_operation::deposit_operation( const address& owner, 
                                                     const asset& amnt, 
                                                     name_id_type delegate_id )
   {
      FC_ASSERT( amnt.amount > 0 );
      amount = amnt.amount;
      condition = withdraw_condition( withdraw_with_signature( owner ), amnt.asset_id, delegate_id );
   }

   register_account_operation::register_account_operation( const std::string& n, 
                                                   const fc::variant& d, 
                                                   const public_key_type& owner, 
                                                   const public_key_type& active, bool as_delegate )
   :name(n),public_data(d),owner_key(owner),active_key(active),is_delegate(as_delegate){}

   operation_factory& operation_factory::instance()
   {
      static std::unique_ptr<operation_factory> inst( new operation_factory() );
      return *inst;
   }

   void operation_factory::to_variant( const bts::blockchain::operation& in, fc::variant& output )
   { try {
      auto converter_itr = _converters.find( in.type.value );
      FC_ASSERT( converter_itr != _converters.end() );
      converter_itr->second->to_variant( in, output );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void operation_factory::from_variant( const fc::variant& in, bts::blockchain::operation& output )
   { try {
      auto obj = in.get_object();
      output.type = obj["type"].as<operation_type_enum>();

      auto converter_itr = _converters.find( output.type.value );
      FC_ASSERT( converter_itr != _converters.end() );
      converter_itr->second->from_variant( in, output );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("in",in) ) }

} } // bts::blockchain

namespace fc {
   void to_variant( const bts::blockchain::operation& var,  variant& vo )
   {
      bts::blockchain::operation_factory::instance().to_variant( var, vo );
   }

   void from_variant( const variant& var,  bts::blockchain::operation& vo )
   {
      bts::blockchain::operation_factory::instance().from_variant( var, vo );
   }
}
