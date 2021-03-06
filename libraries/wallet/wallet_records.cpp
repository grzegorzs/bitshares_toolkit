#include <bts/wallet/wallet_records.hpp>
#include <fc/crypto/aes.hpp>

namespace bts { namespace wallet {

   extended_private_key master_key::decrypt_key( const fc::sha512& password )const
   { try {
      FC_ASSERT( encrypted_key.size() );
      auto plain_text = fc::aes_decrypt( password, encrypted_key );
      return fc::raw::unpack<extended_private_key>( plain_text );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("password",password) )  }

   bool master_key::validate_password( const fc::sha512& password )const
   {
      return checksum == fc::sha512::hash(password);
   }

   void master_key::encrypt_key( const fc::sha512& password, 
                                 const extended_private_key& k )
   { try {
      checksum = fc::sha512::hash( password );
      encrypted_key = fc::aes_encrypt( password, fc::raw::pack( k ) );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("password",password) ) }


   bool  key_data::has_private_key()const 
   { 
      return encrypted_private_key.size() > 0; 
   }

   void  key_data::encrypt_private_key( const fc::sha512& password, 
                                        const fc::ecc::private_key& key_to_encrypt )
   { try {
      public_key            = key_to_encrypt.get_public_key();
      encrypted_private_key = fc::aes_encrypt( password, fc::raw::pack( key_to_encrypt ) );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   fc::ecc::private_key     key_data::decrypt_private_key( const fc::sha512& password )const
   { try {
      auto plain_text = fc::aes_decrypt( password, encrypted_private_key );
      return fc::raw::unpack<fc::ecc::private_key>( plain_text );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("password",password)("key_data",*this) ) }
} }
