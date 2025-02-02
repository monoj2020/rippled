//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2021 Ripple Labs Inc.

  Permission to use, copy, modify, and/or distribute this software for any
  purpose  with  or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
  MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/tx/impl/NFTokenBurn.h>
#include <ripple/app/tx/impl/details/NFTokenUtils.h>
#include <ripple/ledger/Directory.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Protocol.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>
#include <boost/endian/conversion.hpp>
#include <array>

namespace ripple {

NotTEC
NFTokenBurn::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureNonFungibleTokensV1))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    return preflight2(ctx);
}

TER
NFTokenBurn::preclaim(PreclaimContext const& ctx)
{
    auto const owner = [&ctx]() {
        if (ctx.tx.isFieldPresent(sfOwner))
            return ctx.tx.getAccountID(sfOwner);

        return ctx.tx[sfAccount];
    }();

    if (!nft::findToken(ctx.view, owner, ctx.tx[sfNFTokenID]))
        return tecNO_ENTRY;

    // The owner of a token can always burn it, but the issuer can only
    // do so if the token is marked as burnable.
    if (auto const account = ctx.tx[sfAccount]; owner != account)
    {
        if (!(nft::getFlags(ctx.tx[sfNFTokenID]) & nft::flagBurnable))
            return tecNO_PERMISSION;

        if (auto const issuer = nft::getIssuer(ctx.tx[sfNFTokenID]);
            issuer != account)
        {
            if (auto const sle = ctx.view.read(keylet::account(issuer)); sle)
            {
                if (auto const minter = (*sle)[~sfNFTokenMinter];
                    minter != account)
                    return tecNO_PERMISSION;
            }
        }
    }

    auto const id = ctx.tx[sfNFTokenID];

    std::size_t totalOffers = 0;

    {
        Dir buys(ctx.view, keylet::nft_buys(id));
        totalOffers += std::distance(buys.begin(), buys.end());
    }

    if (totalOffers > maxDeletableTokenOfferEntries)
        return tefTOO_BIG;

    {
        Dir sells(ctx.view, keylet::nft_sells(id));
        totalOffers += std::distance(sells.begin(), sells.end());
    }

    if (totalOffers > maxDeletableTokenOfferEntries)
        return tefTOO_BIG;

    return tesSUCCESS;
}

TER
NFTokenBurn::doApply()
{
    // Remove the token, effectively burning it:
    auto const ret = nft::removeToken(
        view(),
        ctx_.tx.isFieldPresent(sfOwner) ? ctx_.tx.getAccountID(sfOwner)
                                        : ctx_.tx.getAccountID(sfAccount),
        ctx_.tx[sfNFTokenID]);

    // Should never happen since preclaim() verified the token is present.
    if (!isTesSuccess(ret))
        return ret;

    if (auto issuer =
            view().peek(keylet::account(nft::getIssuer(ctx_.tx[sfNFTokenID]))))
    {
        (*issuer)[~sfBurnedNFTokens] =
            (*issuer)[~sfBurnedNFTokens].value_or(0) + 1;
        view().update(issuer);
    }

    // Optimized deletion of all offers.
    nft::removeAllTokenOffers(view(), keylet::nft_sells(ctx_.tx[sfNFTokenID]));
    nft::removeAllTokenOffers(view(), keylet::nft_buys(ctx_.tx[sfNFTokenID]));

    return tesSUCCESS;
}

}  // namespace ripple
