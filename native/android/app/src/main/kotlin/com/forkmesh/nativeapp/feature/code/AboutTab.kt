package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun AboutTab(repo: Repository) {
    val c = Fm.colors
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 16.dp + navigationBarBottom()),
    ) {
        item { RepoHeaderCard(repo) }
        if (repo.donationAddress.isNotEmpty()) {
            item { Spacer(Modifier.height(14.dp)) }
            item {
                InfoCard("Repository funding") {
                    Text(
                        "Optional donations use an external self-custodial Solana wallet and the public repository donation " +
                            "address. ForkMesh never holds the funds or stores wallet private keys; independently verify the " +
                            "address and network before signing.",
                        color = c.textSecondary,
                        lineHeight = FmType.small * 1.35f,
                    )
                    Spacer(Modifier.height(10.dp))
                    Kv("Donation address", repo.donationAddress)
                }
            }
        }
        item { Spacer(Modifier.height(14.dp)) }
        item { OwnerBountyWalletPanel(repo) }
        item { Spacer(Modifier.height(14.dp)) }
        item {
            InfoCard("Repository details") {
                Kv("Default branch", repo.defaultBranch)
                Kv("Language", repo.language.ifEmpty { "-" })
                Kv("Stars", "${repo.stars}")
                Kv("Forks", "${repo.forks}")
                Kv("Mirrors", "${repo.mirrors}")
                Kv("Visibility", if (repo.isPrivate) "Private" else "Public")
            }
        }
    }
}

@Composable
private fun OwnerBountyWalletPanel(repo: Repository) {
    val c = Fm.colors
    val gateCopy =
        "Legacy Worker-custodied bounty wallets are frozen for an offline, balance-reconciled migration. Do not send " +
            "funds to a historical deposit address. New bounties require an external self-custodial wallet, program, or multisig."
    InfoCard("Legacy bounty wallet") {
        Text("Custodial wallet disabled · migration status only", color = c.textPrimary, fontWeight = FmType.emphasis)
        Spacer(Modifier.height(6.dp))
        Text(
            "ForkMesh no longer creates or uses Worker-held bounty keys. Historical public status may be shown for " +
                "reconciliation, but this app never prepares or funds a custodial address.",
            color = c.textSecondary,
            lineHeight = FmType.small * 1.35f,
        )
        Spacer(Modifier.height(12.dp))
        FmFilledButton("Custodial wallet disabled", onClick = null, icon = "account_balance_wallet_outlined")
        Spacer(Modifier.height(10.dp))
        Text(gateCopy, style = Fm.text.bodySmall.copy(color = c.textTertiary, lineHeight = FmType.meta * 1.35f))
        Spacer(Modifier.height(10.dp))
        Text(
            "${repo.owner} can review historical public signatures here, but no wallet or payment URI can be created from this client.",
            style = Fm.text.bodySmall.copy(color = c.textTertiary),
        )
    }
}

@Composable
private fun RepoHeaderCard(repo: Repository) {
    val c = Fm.colors
    InfoCard(repo.fullName) {
        Text(
            repo.description.ifEmpty { "No description provided." },
            color = c.textSecondary,
            lineHeight = FmType.small * 1.35f,
        )
        Spacer(Modifier.height(14.dp))
        ChipWrap {
            RepoChip("account_tree_outlined", repo.defaultBranch)
            if (repo.language.isNotEmpty()) RepoChip("circle", repo.language)
            RepoChip("star_border", "${repo.stars}")
            RepoChip("call_split", "${repo.forks}")
            RepoChip("dns_outlined", "${repo.mirrors} mirrors")
            RepoChip(if (repo.isPrivate) "lock_outline" else "public", if (repo.isPrivate) "Private" else "Public")
        }
    }
}
