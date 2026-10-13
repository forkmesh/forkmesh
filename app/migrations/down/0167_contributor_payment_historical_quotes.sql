UPDATE contributor_reward_estimates
SET payment_usd_micros = 0,
    payment_sol_usd_micros = 0,
    payment_valued_at = CASE reward_id
        WHEN 'reward-forkmesh-forkmesh-issue-655' THEN 1787690247356
        WHEN 'reward-forkmesh-forkmesh-issue-656' THEN 1787690212452
        WHEN 'reward-forkmesh-forkmesh-issue-623' THEN 1787673981676
        WHEN 'reward-forkmesh-forkmesh-issue-648' THEN 1787609401002
        WHEN 'reward-forkmesh-forkmesh-pull-39' THEN 1787609374573
        WHEN 'reward-forkmesh-forkmesh-issue-639' THEN 1787609345485
        WHEN 'reward-forkmesh-forkmesh-pull-38' THEN 1787609318788
        WHEN 'reward-forkmesh-forkmesh-pull-37' THEN 1787609290981
        WHEN 'reward-forkmesh-forkmesh-pull-32' THEN 1787609259936
        WHEN 'reward-forkmesh-forkmesh-issue-652' THEN 1787609167400
        WHEN 'reward-forkmesh-forkmesh-issue-651' THEN 1787609165407
        ELSE 0
    END
WHERE reward_id IN (
    'reward-forkmesh-forkmesh-issue-655',
    'reward-forkmesh-forkmesh-issue-656',
    'reward-forkmesh-forkmesh-issue-623',
    'reward-forkmesh-forkmesh-issue-648',
    'reward-forkmesh-forkmesh-pull-39',
    'reward-forkmesh-forkmesh-issue-639',
    'reward-forkmesh-forkmesh-pull-38',
    'reward-forkmesh-forkmesh-pull-37',
    'reward-forkmesh-forkmesh-pull-32',
    'reward-forkmesh-forkmesh-issue-652',
    'reward-forkmesh-forkmesh-issue-651',
    'reward-forkmesh-forkmesh-issue-649',
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
    'reward-forkmesh-forkmesh-issue-643',
    'reward-forkmesh-forkmesh-issue-627'
);
