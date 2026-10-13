-- Lock the USD value of the 37 finalized contributor payouts that predate
-- payment-time price persistence. Source: CoinGecko's SOL/USD historical
-- range endpoint, retrieved 2026-08-30:
-- https://api.coingecko.com/api/v3/coins/solana/market_chart/range?vs_currency=usd&from=1787363029&to=1787697465&precision=full
--
-- CoinGecko returned hourly observations. Each payment uses the last
-- observation at or before paid_at, never a later price. USD micros use the
-- ledger's integer rule:
--   round(amount_lamports * sol_usd_price_micros / 1,000,000,000)

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 97925273,
    payment_valued_at = 1787688000000,
    payment_usd_micros =
        (amount_lamports * 97925273 + 500000000) / 1000000000
WHERE reward_id IN (
    'reward-forkmesh-forkmesh-issue-655',
    'reward-forkmesh-forkmesh-issue-656'
)
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 98330683,
    payment_valued_at = 1787673600000,
    payment_usd_micros =
        (amount_lamports * 98330683 + 500000000) / 1000000000
WHERE reward_id = 'reward-forkmesh-forkmesh-issue-623'
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 97477072,
    payment_valued_at = 1787608800000,
    payment_usd_micros =
        (amount_lamports * 97477072 + 500000000) / 1000000000
WHERE reward_id IN (
    'reward-forkmesh-forkmesh-issue-648',
    'reward-forkmesh-forkmesh-pull-39',
    'reward-forkmesh-forkmesh-issue-639',
    'reward-forkmesh-forkmesh-pull-38',
    'reward-forkmesh-forkmesh-pull-37',
    'reward-forkmesh-forkmesh-pull-32',
    'reward-forkmesh-forkmesh-issue-652',
    'reward-forkmesh-forkmesh-issue-651'
)
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 95054472,
    payment_valued_at = 1787518800000,
    payment_usd_micros =
        (amount_lamports * 95054472 + 500000000) / 1000000000
WHERE reward_id = 'reward-forkmesh-forkmesh-issue-649'
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 99214580,
    payment_valued_at = 1787374800000,
    payment_usd_micros =
        (amount_lamports * 99214580 + 500000000) / 1000000000
WHERE reward_id IN (
    'reward-forkmesh-forkmesh-issue-586',
    'reward-forkmesh-forkmesh-issue-587',
    'reward-forkmesh-forkmesh-issue-588',
    'reward-forkmesh-forkmesh-issue-598',
    'reward-forkmesh-forkmesh-issue-599',
    'reward-forkmesh-forkmesh-issue-600',
    'reward-forkmesh-forkmesh-issue-603',
    'reward-forkmesh-forkmesh-issue-622',
    'reward-forkmesh-forkmesh-issue-624',
    'reward-forkmesh-forkmesh-issue-633',
    'reward-forkmesh-forkmesh-issue-634',
    'reward-forkmesh-forkmesh-issue-635',
    'reward-forkmesh-forkmesh-issue-636',
    'reward-forkmesh-forkmesh-issue-637',
    'reward-forkmesh-forkmesh-issue-605',
    'reward-forkmesh-forkmesh-issue-641',
    'reward-forkmesh-forkmesh-issue-642',
    'reward-forkmesh-forkmesh-issue-640',
    'reward-forkmesh-forkmesh-issue-601',
    'reward-forkmesh-forkmesh-issue-589',
    'reward-forkmesh-forkmesh-issue-590',
    'reward-forkmesh-forkmesh-issue-602',
    'reward-forkmesh-forkmesh-issue-628',
    'reward-forkmesh-forkmesh-issue-643'
)
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;

UPDATE contributor_reward_estimates
SET payment_sol_usd_micros = 95541493,
    payment_valued_at = 1787367600000,
    payment_usd_micros =
        (amount_lamports * 95541493 + 500000000) / 1000000000
WHERE reward_id = 'reward-forkmesh-forkmesh-issue-627'
AND status = 'paid' AND tx_signature <> ''
AND payment_usd_micros = 0 AND payment_sol_usd_micros = 0;
