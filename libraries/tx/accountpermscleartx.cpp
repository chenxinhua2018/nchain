// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2020-2021 The nchain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "accountpermscleartx.h"

bool CAccountPermsClearTx::CheckTx(CTxExecuteContext &context) {
    return true;
}

bool CAccountPermsClearTx::ExecuteTx(CTxExecuteContext &context) {
    sp_tx_account->perms_sum = 0;
    return true;
}