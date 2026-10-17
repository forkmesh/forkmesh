enum VaultFixtures {
    /// interop-from-js.json
    static let fromJs = #"""
{
  "format": "forkmesh-vault-interop-v1",
  "suite": "fmv1",
  "madeBy": "javascript",
  "warning": "Test keys only. Nothing here protects anything.",
  "password": "correct horse battery staple Å",
  "kdf": {
    "algorithm": "argon2id",
    "version": 19,
    "memoryKiB": 19456,
    "passes": 2,
    "parallelism": 1,
    "salt": "yWWJmZSnb5MstO0vcj2fww"
  },
  "masterKey": "ff494b1c99c6936b0fed6c847cd4e0254d3c018d7510ce38862dee172b5c0b2c",
  "accounts": {
    "alice": {
      "keyset": {
        "v": 1,
        "vaultKey": "Qb-alG3frXZ9ldduWDiYjOXs04aC_TAtYfSUbxb91Po",
        "x25519": "8O7kkakabEKbC3DcV_HJc6W7dnsQrYWtC2lyXtLhvmM",
        "mlkemSeed": "naysiKO5cZ6QVvs-VvCgHxqiz91C5qvepTOuL-P47LyxRrT2uIDXRVu1cZUmWtZjg4hYsdBf7J74ZiOWEpd3PQ",
        "ed25519": "h6bfFCnbOBLHdi0Cm5mw-tgC90vkRHOB4Ic_-l-srK0"
      },
      "bundle": {
        "v": 1,
        "suite": "fmv1",
        "account": "alice",
        "x25519": "DSwbZ-fpEI9TgE--jJ1bEcMALgod-VYXB8BHs8Yey1w",
        "mlkem768": "8DR2AuFIvlmZeAKpvNAZpvFuxnVv7OAMb1tLH0ONecl0qeB9ixu1ckRb_xJrq0qP--GfWrqOdqCvHVM3m7gXWtxjlmIDQqN5EDG0wWB4LTlsGlYEcxKzckXJbFCYayIBVZFhIkp5ElwpzCxMjHwiCqGEBpEg6iRL9ttTI7t2juUgsHBmgEB95GJcYcInVZag9MN2FcQBYFgRCeNb_7oV2Xh4JLGiRmPMi8cYiUCf4LUMyfApg0SkO-qZ4Du--MsE3lBdekpZfydD6XtpbUFyCcI-S9dj_Dl9fxNYkFdRQASz1wtcN4NVdUgslsnEg7MxOGt8hsGW9gJ9VuovwbOp2KGu6KabBAp_NQsMzalz-GhVpKkyLvDGWvNkUrBzNsbDfdWSj8cOcKYrQ4OAm1ydXfSfDYgI7eaJZEU-UMIvRxph7ruE0IeHKja2RiJng_c9PAVXN_XPPJcu2_yiUkuVjAymfROgNdQBa0eUVYdny6RoPxmmq6F9InK-isNXKwaO5xQwtPC_vNI9T4wUsOAkg8kARxQYyxCT_3ApF7uIQTpHiDp_VXx8lLoAHlN40sM7HOEcCEJfoRYLtSU0BZweWEanOwI4MTCfIBpJIGtj2Gq2a0pewniFKfh0dzq1OeVl4paZl4O5HPjKQXhDfZaT8mbNJeNeHazMTodNjzcRJ6LNQrsQTtWjX6p9Z6snS7S3tPRP1beigwsNz1FZjlUvNtQB59eaTbkLLiqmpamoMQBY9hSRw-d5pobPj9O5dEUfQ-GyVGLApEZdD8eXZXFJE5fM1exdB7FF7pQ1eXN_JwVY4aiKN-pJ5vKxlzJPevYh8-sRX1C-GNSWn9RN-nOwnMISZyYrIleLNTyyOaQbXHOSKYUBnAwfWiVwRbUt_FONulGyVXiZDVwsXqRkXCFACqIx1WBZWpB1OmOC-grQKBdV7TI9bXHJP_kqk1i5HTRdktKvZFNv6kU0HwVJj3g60nxxFNSRkCcCBURi7tIcLGo-R9WXv8e2DgjAfZBeFlqw_Hx5n1oPtHsPc_uOr5VTUbQtNIR0NBpfftk50KIx7MiQTCOejhZowHAsCTu5e_RYw_qS15G2D2aI8VQDmykN3dR36Dc7KRzCf5gRApR4PhN1yTKQHpUuc6ehU8C9p1E67AliNmNq_XtU_aVZlnlZLbUnDAEHZ2AgQZW6srBsVAQm7swoqMiOJJgRJVK63gKDGlmHb1nCzedLcjcUjRQ9sKG-hkwKy-ZGY-QOXEl5afuzdOC3JDnJ5nsHXUNcIUxz0Ksqd7UGKVYot-ZFcCMrz7GLmnpfWlBK19i5DFlK_VlhMqiUgFvGxzWOttJm6FMgDOzNZpZeKTwTPklYMply6cVmvjzDL0RUU6DOi9expwxQZstndXoF1ajCK8wZJxx_RbsnWncktWWPEqOvwrA6FNcRfIw9Foln1Oa7gUtyjqYmzOWU8RCZdoFy6vwdOOvDTIRRygs0lXYnUcUFozqlmfwc25nAM8gPo7W_OSGOSQS2g-UJMKFWRAQQzEkbCBQ0rFM3uGxTzEBfbYmwklb-2JtN9erFe3Vr1S0YwSSb_8w",
        "ed25519": "scYq6TDWCSmQQOTNeFjXCbOTW1-k2GWQ7XIt5tnTfxw",
        "signature": "AZqi9CdSByIu5mA7opRISES8kP2M1dv6Jj5OW5YCxeix5M9_2DKWY43G28rHLr3dZU2VSbRMkTbCZfdUkQWcAQ"
      },
      "fingerprint": "edfa e39a cf19 1f54 aac4 1f60 0abf b889 0ded fdf3 5636 62ae a3d0 4056 9b46 87ec"
    },
    "bob": {
      "keyset": {
        "v": 1,
        "vaultKey": "ZGbVz9xxv-UK_ZjMtDnnc_1DRvDIKARFg5ZPEV5dm6s",
        "x25519": "wLDceTbrPiU6SY9jB_J8z5MV2ifwYz6b_qSrAFned1o",
        "mlkemSeed": "SIVji_Bys4diGYfzjnuMn-qJraFp9pPaeioOGlwtt7DpLNkdqHwlky9da0xpD4z0O_5YAym-L_O_RQw-4hlqGA",
        "ed25519": "L-OiJvmn2zPoX3Qq7L85_C-POVUn8GDDGW6vbTml4Xk"
      },
      "bundle": {
        "v": 1,
        "suite": "fmv1",
        "account": "bob",
        "x25519": "XiE24ymr3qNrWXt5W5Ux4efOtg0istitOA7h75rNJjk",
        "mlkem768": "RNZ_tNnL9gbKGtpK_cgrwmdip5iuCnbJPBxBGaN7-Nmbq_howVgOQNcSiIBSd_t64hO3_qxQlwE2CjSeLRKvFFao4No2lalSOIoRhtjHQvqFUMEzuncrxko4t0p77LEnIZVJ5nJsLMtBXtE8vpSzcUtY6ySa5Gi26dBfSjtd93M7P6hM_DXMgqG4DYNjxbc_L0fDNiyqpiNZstxKWNpt03B9-Zpe_pSM3hQIkrpJyEwiH_oEGsgdRwRzlxwf-jtXGZlo3Am5jcBDJQkhooNyTyVVCcUWWHYFhGzIdoZVHgR2NoAoUJpPTMor-XN3jqOD5Sgr_cVK_1wpfnOGjDY_FzHALKEA3gBjX2Qxv4KAnGsHggqIdhmr68JoXUFvULGXDLSjXPsoIbecmGu1GFHIyREiIEsIqYmW8XwyiJSe38iWZeFr_pi9RpBnE8mkY5e6J5GqBuabgcJXEkxFVDl-BBCqZlqyxehPyRClM1Zh9gKlIbgKn0B81XJ7pEvLG7YMcsRav5OAPWpzjjG78DGEJoeaytRAQhRnTieT6uDInjYD1IC3pEZ2kAVpjYBICFiORygvekQsWOZh1IytdIhx0aYudkmIjlrJBMSE0klBzDBbPvhRmdjA4Dg-0ALB0TzPVoRJRXi6ZeIk3CStpywQIPHHc_FwmJmpSZXNRHQZKslDrkZycSeCubFJkPqeiXdQD0MVRBeyF1Q8NDDClIUWcryHLVu1ebGwhyVAo9yolpFqkQe_w7sQumPH_lx1TzpHdVKOAzszDDNLTtzM-JANyPO0QNNwpDOzjLjKjBQvqUo9WPdV3EPLh9d36EuleZoqMDnOYalmWeIVWpedKjC2aPtduGipHhi1GrsrX-NqXgNZVyk7kUPLWBI6WgCLrXRp3QIMF1u6EDpwHFW2CCeIOmF_QHqhD2cRRxBSI8JTSFcPUxypnUGoglwwbdBSwWIVulyQe5E3o0N9UDEiKXW4dbnGgQQ2SWgDLWIeozprt0oJ43BshnBmf3k6uPBSE4eTs4Qp6EmmZQO6WzkbJmJhQdF22WZygElDmimmZjmXYRrDTlUqfciNGfuipAtFKWZFQqg9vMqA_soMGjC7W8gqn4dQsMtKgnw1-iJ9i0Sx6CwEjJdWd4oCdmaMujZV0iRW9Vxt1lOezfKkkDJj5OdLJCxPIoIY0GTIUFSq1Gad0_sW1OqNabRHsztF7JO6dOQ5NqI4eAAJnEZo8JYF3UGZ4YjD5YUojeit9up1duZU88RdHji0g8y8SqYDw2GaBWqqhkGZmHxEOmvFFVS0VEKkO3AiSWq9aJN9WgzC66J1BVNprOgh8NgqvNpKygWoYRKyz9xiu5oPkDeMDQOmLbsIB7QC_8U9U1kSaXLKnKqdsKoahRC2DNKQBfZwOagisRFpDNeNUzKzg4ovdLMzmQtUYeZYvbDM9-Mjo0AGQBoCLZtxjmg3kVKp7lg0BzeaLVYch0Jm-yKHg_McUqBzzqQ5oBnB9iO1orm45kYd0KV2RDFlLUuk0Tx_SGEJvMFye4NRORmUhtxqqPa3LB3u86YO5ef0Wi3d-lC9j8krFVy7Ge0",
        "ed25519": "xnD3S0bRO874BMVCHv0A1iAtEA8wu7al21CZ605Q7_M",
        "signature": "Eeg6rVYrSkTjA936iFG46TUWNZwzTf_fZAKMBdZ6RtRe2eKT2CaD00zQElcnO78nmdfQJVALNq4cy8P0z44PDA"
      },
      "fingerprint": "90ae 3e15 711d 38d7 e28d 319c 2243 0dd8 19fa 1f5e f279 f9de 3a73 4d3a c641 2667"
    }
  },
  "wrappedKeyset": "AHeyuusPKFJdxm_6r4KVlXW5MKB19NWs2eZFcgrhZszNFmI2Rn0l0iOFYD6FpfK02UmP7yv7blPZhgA61mkHSILRQwz7UlCbgpCH13IWcdG0VFzzPA4HpHIXnmCmbGLYVLo0L-IBFBLfFXhPE0z-fEiMdtHYUZi-rZuZNC6uCWza4QVanzvHsvsJEk4tg4k_-voKWIsZgMJuc75GpPpRMXhAd7fDOEWw__y7iT-2hnrmmiYR9KRbBIzyTYGogVyZR1ja8i4ZSbA6rlIkuoyNXjU2A1HfO2gZ78xT9dTE4ZeLLFtGazqfkKEvPq_dNJsKn-z4YSnxp5KJ12135NOPdM0qTdjdAPdrtOOfOIThGYFectTAdkFJdOlc2EihJHcmrchh9OgBH7iAHxH7SWJRPw",
  "recoveryCode": "WXH6-ENYA-N90E-0XYR-9G19-9FCS-25FK-WQTT-T1JE-XVNF-JAAA-463J-R0Z0",
  "recoverySecret": "e7626757caaa40e077d84c0294bd99115f3e5f5ad064eeeeaf9294a21872c03e",
  "wrappedRecovery": "D2Mjpiux8AeYtCkoLabmro5SoEMYCVn2TCIkqMVTYjrZnxB1jGlxa-IIZeWsm0PuGFUh_ydcLRwIRF7DbMkV65-lU3yhyYeD-ot1AZHTYRjn8sJDpnir0NQ_FXxOrNQRDkuKWfzwqhjA2LG-0XqcguUGuMHJNwKXu49Dqg9VXqOgdTbt412Dvq5VEk4hWV4N5-FfeO9G-AS25S8nywzdr08PZTxs3ECGD0qQmy7i0Fk3iS8V-ZGVx3_feYjLaOyOb8PaRA3SemvhgJMbCJ_Vx5V92KOMsfUwE-jaRiFglStN3DSlJvtndnFcsAKO4UavIWFBD9pCii5m8oDhfj7SMhQeo9XFrNhsYg0ZIOZyRqPhIMLeW-A9Bw56V8QabsbOiugo0C6Ft4996eOVeAm_6A",
  "item": {
    "id": "3a84b9351e9e75ef776a324b83518cfd",
    "key": "937affd9eb22ea1c7ae29de6da479916deaf49af029a729573fd56ec5a85f4fb",
    "fields": {
      "title": "Registrar — prod",
      "username": "ops@acme.test",
      "password": "päss \"quoted\" \\ wörd",
      "url": "https://registrar.example/login",
      "notes": "line one\nline two"
    },
    "body": "T0DLHd2fRWa-4jo0VrLgl5LNxjiNX1viUPJvWeP5YoB7wibGbG4KBY0H-j0YtHo3zMvTGoLYNKEwL60eDWsfDy956dBFQ_Kkae4ZfiZyMZBARiW7s8SmAqwZT4ZKZbFvMc35a85EvdErb0FZB3_VWRBRpFmnE4iZpzGSgsnb382sx1osiA0eXaX0tqPyWJGZmBwFE6WX56pa4zANGY6d5HXzon3XBTHWZeAAFOKVeMZqP1J0ep1rxqwFoGuzSDRZcrIfRseDVblW3enqcWNhKnURLyTOoaWY45ls0YcnwvOc3yMa2Ju6RZ1H1bRy2t_kA6iHPw4tlnu1OrMnV3tY4GW98lGHJdaCyq27bWgOG_WcFzARuifyxlHAmBQ",
    "wrappedKey": "1--T3l74wI8u8twcoVwUY8v2MTAEbojqSSj1SlA6K3I9cylpUpi7Wmxmw38wKBUVDzoC87D9eiGMCJbI"
  },
  "trust": {
    "value": {
      "pins": {
        "bob": "90ae 3e15 711d 38d7 e28d 319c 2243 0dd8 19fa 1f5e f279 f9de 3a73 4d3a c641 2667"
      }
    },
    "blob": "1OU_nGGAvxlQSjT76QXpFLPdB4EXpHMRRXhSBxTQ_jnF0Rufn7y6YbfYUi7C3qIawr6qmCoEOFuHLKPA9QYL0hJPIgZQp9iv0D69fGd8lTBdGdvYkk8oOCoZ2RDXOiP2pLrlW8w_3McWwiRVuh5FNYeM2eHtbChk5VoEnyq1"
  },
  "grants": {
    "read": {
      "v": 1,
      "suite": "fmv1",
      "x25519": "-JqfCHPmEk2e5JUHsaekjO1cTVKp5_XOXxbxocYApis",
      "mlkem768": "PTiGfvNX7Q8gvE5_hI7u1QVZZYkFeQmcKLfCJnGLWTLPpdO5AMuUYYcVxmB1pfiDdnbLM7qlQszHRMbGj4e85Mdv1L10Y9kXBMzGm1mBIjyPzB2mZeriqmL_GX8knm7KcMACoM6n6-gpNJi6_JAEZl8vAtbFV0sdf2HEm79oRo28-w5efTqA5uu-2odJbrYgMSWx_5lsKyKd861gTinZwiaCE_0xZapYoPpn3mzjQc_U7A11M4y1hsLAFKp4k8G85fzUEpPV5o2UugLiqSYaYJHQlkgAEW2FXG4T2quAaqAnRmiYrazb2uTQu3hcwweBDcI7dyZpZafdQfmaZPgPT2CVXg6hADgTBiQ5yJfbmIVCcWrr2SZ-VOwfHD6XtDWH3gVvgkn3HN1z4ZndF0I_h_Y08vm35FqT8zwXgrvwp_RtsCIx7J_bmE97DKfKmadgWg6iQ2gogFnzs8w3q6mZdOr9UXfjAti4aIyDauuxoRl5TjKqsGsWtqfhQzW8th2GEhb5R0HnM_FLY5CjiU-M4t4xk78e47qWGt58s6-Lu57oSUTJv1sxLN86nRHluPx59mtRR0DumnZDIFIi2xafcDVQePNkXf0yfQY1PA85EJI-fDeXm3YdFF5dk3AbVEHlejBxt06MfsUEpSyEPc4Hu5pSCvWi8o49zYaU0d9GxIvkHRhQkL2GXdOWDHYG5nGGoaAtB78tBV56zBr0JxlASiKJr9brYjVWnRScFW-8uzbLGQifS6PQzqabVZycPPukbqlWbJlk-CTT9UyyRyXibUxhw4YoyuWfe5aF3S8yVPEjRArqpGguBvkbUceIgs-Qh3DBeB_lX8Ry87PgJ2Xh2o2JY93GmMcmSKGAknEN-7CJAFwwuypKMsJAv1-gn4qm1l_3bqtHUJuXXIGpZ-kCGk2ucdXE5QTO8_9KuceCek5f8sbIRWDSWutxwSMuwBz-MBy6LKTo8UJ6OgFP0ErR8WNzb5gnJYCvcxzDtTbEfV_330qH8tX33Z1ni1bQzXZWxrcu-mA-YcqYXNHsmDJg6pDs1366bWNx7x2J6SsfiA8JO-HOePlIUW7MeuZ-hTuL_vBpbrQQeJblqZnvsneTMzrcTysuLFE6MR2QoP3YdhLsU-YitjG0cGh-O3HNAvfVYDXSCA0ojZTexwg3kPHd2TyeTkMHCiyTAry8XAiGwxuHVIzS7ztqiPetCXAMDG3Wi4C5wr29VVrF4YZzKAacSAGFUXFxERSETbSUTDFk3MxQlVXeA2XMALQckEYt7LmaadHXf3VGQXKp28nY18bEUqGKBGK-kKQR8EJSo-UpKdjfXFEw4IsMWVgeiH8h2mnnUBlHUDgcGlcdwafArw_V1JVvaomu84tnU3ZRq1I8SqCyuEmHW2ZjioXnGPE2xy7pQ_fu45PVpNzKLw7FN44hk-nwtYIdknAcCukN9741Y4g",
      "wrapped": "WVYZOmnmOvQfAYQWaY6KdoWGgeBGdfpDQiXLSBlOUS2hrOwQrZ_XIY_AHz8iersRr5s7rOrxVPszj7T7",
      "signature": "pM1KTs7fpWS8MtdrKcyVGYxM2UqW21UK0lwE6N6ZNz0VpNzD3rNG3KOXuvoV6-KoaAVDcFT8N7M4elne4PZJAg"
    },
    "write": {
      "v": 1,
      "suite": "fmv1",
      "x25519": "aw4V21sTwoHeUC70vAOisW3Ab7pCy7VNMUeh25JNKkY",
      "mlkem768": "iUsac8hhQEnRTraZP-urmuhvYvk2BwYFKRPfzHax_StyTqBS-kXgaRna0PokZjl6V0i1xlNP-0vIm9Q52eUSSqKTmmbCsG8mUarvIOKuruU9UNm6IARzZAbnKGhfB8Fy4v6ZBWJrxqh-4cBxNN6miwbGeGezvOham7PFqI0Aj4LwczMBah4DmGYqjrzXXCv9ATMYJS-ECe6MLR4oKFNruQJ-SgCDrBIrwpRJToEexNbL0ZPiaWulqmxt-7-f3rRNq8Y7LlGj_ymxeXv0X-V_hyGpEV0fuQTXJUH0l_NQjXwk3hqsEvwYuFLLY782b3wnCZpuQCmovw_czt59Xc7laeIm2FPLTIdh1DYjE57l-0JZSgUcSV7yDe00bC6iKTKHWvIpL_1xrDzQpFt2pHf627OWOvEI75sGJNSCTVlNdeTUdHWM163xDTOx9g_tYbwPxJYScmGLW23xqLXVL5V2iRDadRurQczKKLJii03J2wwjIXonaTO0xDFBDMyRejIr-bZ5ZncTuq-_n7bCJNbJrczq0VaZCKHGgvGdaE-ymIE5kc1o7mtUaWhcZYN8KzL1jncaNRv9yszNroGcJ5JoiXsurlMVPFpatPXRlhUAkhTgdcO5T78RNm5NzvKvcyAc8fCncIU4sEbUhZwX-uo_znusTFmKR6QjX_DMpakozmkBfySCyq3yPCLzua-fiiCtkRF0ESNOMxAZ1iezEeo1KMMctH42_2XFRGPSv30rNKrOUj1B6jlIXDlfJ0IB-3dYX_nagcLnMxpB2FVNZBBCcLtaW77Our8asAWjl0OgFNs8BRFRCCAmeaOaSwdMia0pKIEK23H-SGG0l_OgojaAH5ERSWoQMF1Elo7x4FR-y6heGs8nKPgzk-puqC7kr87C7DE5ETkdm6Q3Eyj3mfLz-udP8KY7M5TrV1yYhctJJBHmJumnmUVr7feKcQzBSATy8EegRPDpPXZHFEenx9CLmXVV1SHgr4vJ_OCILSdb09qstiiE6gGOrWL4m6kwNUqRARr9EQej38SPyh_gVz-A7GHBgXpCQToo2aJ-mzNIsDyDnTiswXFvfU-mKXrDHQMfxA9yiQXGf24-J5_L5o3wnIWBYx6ElB7DDDNneVHRmcyJodr28XpAyNCVirMhB8JEawP2zY1aQ904OLDUrZleJ99YM7j6gujayRPtyf1N-m7y3tNPQX9THb3MW5Z7K5c40JFZoMJUW4v5lWdTQEzEDalqUeRRIYdkDdoMjP83m06Kfh-G6v5UUaNKtRk1uB--fps_cmy5RdV46c97mbO5XCAlc8z1_qYfDQrT5hKypXZzkK-pTnIyIdojeIHcFU9Ol-bSn8lD5KMN9A9shmG0CnbRhU6f7Wt9BZnkd5hzXeCp_h3bfndCWJp5mfGHeFwjO-M--pCnxALmA-R_mL2MLHxN7nwWbtFhsnUoXXynvYY",
      "wrapped": "WxuJovNAENuEmJQZjn_fpCAut4rAfSkHfkjmovXiVIpoi3G7KX6snoCWg2DUar9iqmPptRDxou3jUvwp",
      "signature": "9HkH6ifbjmgfB01_lUaY4rYlrYZf8EH3uVjRRX-Pylh0tNKo_K0pGDtnI7cjJ5mlDz1cicvOpMZ-VJl6qqyMDg"
    }
  }
}
"""#

    /// interop-from-cpp.json
    static let fromCpp = #"""
{
    "accounts": {
        "alice": {
            "bundle": {
                "account": "alice",
                "ed25519": "FqhLWlE-9EDYviVZ865eYQnq4PyIAEBhBcwVEa7EFQg",
                "mlkem768": "YVm9amJMs1RwM4LBpJWvxJI5N8o1qYzLAQhXx2dGTGptqqws28INTQglXYUqWWqOGqs4raZUPMxtvyRVgHJjIaKHGcus3PIirWxbdfVVexqL3uAM03mIOOjCyjPHo_tHuIq2_QKdO4cRCUJcmgFcUzxgHzWXHtuSLLG1gMyAl7ktUXBF0mozJvcsj5KKgbk4w_O8inq97xJFSSojjAWGzCmklMNWQQcgbqYNcgIFxRtWFxtwvHwa4wdOq1hD5_J-c6yVUau76QUz6xBKvuhW2xZPU3idsuV3adumAix_uQme1IkwT5DCJ7KXMEivOLVEbyjKj2cn-qMMc3NrLbmGhOAmzAQ45PCU5chWMxUzZgsACMA1B2YG9RJedewoFwZmielMtaLFWHuDODJKRBEVSFePLxi_wSeyGpojlteOb6Rvd5gUL-Bi6QaDoJuLmvzKlpoNcxukZnWh_CKukybNV1o2P7AWg-VKmbfDxxUJJqtDqftL8Klkf7WLIvVEH8qHxSqF_3BYUFGLFLaz7kHLI8EDL2NwUMB_kXJvrHZWzWGcm1Zc4gC1IuGRwdCjRHTD-vlrADM_L9OKYoGylnUok-y_GyfNdRgDqGUuzUQn0nlb4ZmumSxISAWsSPwNABzLyEyoPKTJNUYVh5ATcBNjOSoYnSOdL9tQbKSU5ALKC4OMbSQcnOQQORElbyq2poCb7JrI6Ee8tRgGq7vM9mk59sRQMZFCBliT7XUWh4jMjry-8ms5drEfXKU7VzFh_YujWgxV3rsDOlM1A4xD6wXLY2atxGkzK2k-iwh-W1dETIsS30o8o2CQFVCWRFOgfjQ5DoOlYcY2gUGr7FlsRnd8xUKxdIsRU3sCPcd-EpeC3pu2ZvyxUdWMgHtX_9uWjRVJEsWniicrZ8Jq1AMg6NMSlsu2qwdwoliegwemKnwN0ggqG2C3nfg7K4AOOaQrG1LBq2qO0zcwF3DMa4Nb_oGPPXhzwLI3unnDwFVXeYq3U4QkXODHauxEBkRz3YJKImetKRHLizMbCBhIyoB_fvk5qwUWg9GhmFY5WJNYK9xHGqGRDHJHdzmN6VZRwBYagfg3WXWzGioYzVfDtmChryURB_PDMKNPJPF5fys3j0HHVLpmdZsU1WXOXzRvfZMzy9cdDdebHifAbLxMesAHyXjLwXsljhBSljpQ8gnOOpXA1RFi-Rkq0Ps2yUJ3vYWChXynfYy9GRxn7UNwp4a2OiQRx-fHVvywDQha9tWHjcsHA6RK4eWHufRJgCzN01AspaYrkokokMQKlLiMnXYRJfzByxAVZsYnzDhur2SZhxRNpOTKgBBIOoWkRQSCncqgx7o4e8Yo5HkOIzGVBPK7MOtz4Hy1xcqPo0J0GomDtFjGiBi0HqitfLAFJrhrD1crLuwvOuHDfVnEk9Zo-JEb4gYoq6mQZEJI54VF46Yu8sodkNptq6uquFC0LqI1-iFXKHE5Fkx0jyOHVcrCedem1bJTmXJNM6yLDLCHqPQivCNqGbCW3qEUoOBg7EQrHlSZgRW7LoqRzl_zRKuoDGiwvO_A5P0UhYOwQ6OkpJQh_ncXGo8",
                "signature": "5rRnAJ47uJEePAr_ClLzwefYLKITkmaKvK4WCcjeejUhRHXNtWWsjQfBiekxnSDhq-CY0xoPi5GrtnO2J9iWBg",
                "suite": "fmv1",
                "v": 1,
                "x25519": "U3YyiELOQ6Mi4rb8HewC261kmXoEETFoCVcjn8gswS8"
            },
            "fingerprint": "b231 6a9e feba 682b 2718 1c44 f543 b509 4411 48e5 5486 d7f6 ee92 8515 8237 d700",
            "keyset": {
                "ed25519": "qPjr7Z1E0Sm5kp980DORSoAbzU1UCamnzu6S8Ehc55I",
                "mlkemSeed": "blDgXeJdwxsEwL546bZk6PW7ISoQ1MZJP4gTRoMWLYFvM_tJkZxggAKeKrQGr6WB9SA0Vdhj4JCqGoF49Du_NQ",
                "v": 1,
                "vaultKey": "cb8w17QtLYOk3UeC9qLrt6RviOqnYg54N70GrDR2IT0",
                "x25519": "UzE7N3KhgFPsFei21zLYalf49wJqt000yoSMRlO75Fk"
            }
        },
        "bob": {
            "bundle": {
                "account": "bob",
                "ed25519": "w3WahMh2FaYvaL7txtGeqzJOkRXnASHMs_lyhm_T-sc",
                "mlkem768": "Q4Emw_sD5aUg-JDFiyAspacr5psnpnIh8jojU9jBMyZQuBFn1PZf78ARpPgIHuGq58B0ltB2C-a-pLySFrKnvQCvHFYSh0GdBLSKEDmRgbuZ9zKBF8Ys-5it4yrDNqqKMaTKErOGyxw9cANDQFpba5LA-hh3iJCrZhJ6V5qkbRptesFsYKRwYWwNXqEgLPxc5VUv4zycv5GSiUCv-cHFbQtK05CIm5I54NBfQXx0i3py4wsWc5oqx3w4uTYjzymDGQA-DSmFpjdEQYFeWuMSXySuaCpZH1Wb_HQSE_RJUlE-vCozAoa_A1CFP5VHxrBpMyIMAWGY6buOavSM2uVeNmNJRuixdtV1zpnPlNVx1ZQ00rKW8JA5ztUBNqYAjkcIvaJTnswFXAPGpylHQmFCd4Zzk0S2zXKQwHqkVOE7vRGrniKlEWCWy9NVrKwt8Kl-xfTGSDZXbIFIqnfCOwUJmKzPvgvEhQOe3CoqolhR1aN_HsdtCkQE07avVHzFBPK9VOE8bUFlqJVI5cSTGVlLExdMaTWAj2m5qOB2X5K7VWyQS5tQuuI2zwdg40UI1TNW-roRbnC2nom51FJH5gfNunnFzXyBLhWFvdS-ePPBcrDLjkyiKxF1aTTDUDIMQXVGeyisEqCkB1Ep0aCdGLqqsgOP7IM5wydQvGFpMvuklixQSTQz5aRxOldfBWVJEEMBboJYn8ZnaNxuN1V73zA4JCUBmrrJHmUEFUyhKjik7OB4h5EfPVKOwfxN1lM1zzIOyqoj6-UpHZNq-whgy4MjKgXDWKBcUBc40jqVkPpq0EfHUBoaScKYtPw1c2lOS1RYxcFiomQotflmkqo_epdUxodWuQMgakoxRpZaGRK11aY28OXB9LZT0yUml8emCAOxtkMx4PqeobyzrxqrT4JRHgEH1rlXNLCQy8i3uftO2mSnZzqoGHXHU4S3QqJupsVkqylmSoM9-2AdEbwlX_JTkQoT8ZIBUSwGcDSr_LOU17Z3UvfMcXZXSDsei0EZhBYE-SNR7ahEgWS8EDanNHEK6wCW_JfOnHmT9guT7lkgL0GxavjP9qKidciZUXxXEAywHuCyy4s5IYxfcNqEAmkgoqNhhNB1zszCJ6JhJ_SP2iGaXus2k0jAl0FqvvA1StqsLdaZEsZqD4pb8MrEelJ7RteQv9oqRVt1j1ALC8NVJYSzayKW1nPCpLKyAhkoF1okTqoZdmdqA-E4rQajMKNWZOM--9S690YHkhiIIDYkt7wnZaW28xRK7ii9AF1T9LsVaaN9NjcbvKlfx7RZWaRu4sZQYdu4yzYx9lQV7-sgLSqXqDZYpldSOvwD5RY2pEFptfApHel7mwulWtZBycKDxlUftLmX52JOTkU9-kYumQpnNAloo0Aod6qwLGyntxOSbdRcejeVBOwtUqOKnZCaoclOxXODmpTFr6II6kGsqOg8NRzIajSGt_wESCNlfBWZ6ovN4Ge5zDCAQnFuFAGMVCXO1dAQzhPISXMXIBYEOlMegXpTIAkQCtGICsKQT5ksCMEBwCgnPmWavSkFO-su7MmppWQNU7vL6paWraQCVPQ",
                "signature": "psHQ3GVFjioR8Bow4alVz99zI3JBf01WXSRkzJelp05BQ-KOEDYicicXlGy3zisMBJpOgXoOImPSaS4xfSVKBA",
                "suite": "fmv1",
                "v": 1,
                "x25519": "bzMCk_7-QhrA4u3O1ymUwDgyvbo3ij7UP6xh-GD6Pwg"
            },
            "fingerprint": "b61b 114b 309b 8c63 36e8 dfd7 7aeb e325 b600 8cb9 2875 07a3 9e07 fe70 f25e 3eb4",
            "keyset": {
                "ed25519": "X414qM9QbO_rvbxpAz3BWlpX_M1uxaBraXue13Gb4jQ",
                "mlkemSeed": "WjEc5xzCymLzPpBMi2pC-WHBTWqMB1VTQY7gtctbcNPDu894odRR6VhzbVVBXwPeE5M7XhsWn7nTFfbRZM1fDg",
                "v": 1,
                "vaultKey": "Vb5WfqVgD9MNHohVNlIovNTmgOuz0fwHGt12oU775tE",
                "x25519": "c0zSMOHsONmSYv89QNdvFfRoP6vBssWuEdYrCYIRy0I"
            }
        }
    },
    "format": "forkmesh-vault-interop-v1",
    "grants": {
        "read": {
            "mlkem768": "Aed8h1V-XWLSEiKZuezXdPXt4FK2nb6goziTyKQkFAmdhZkK2k7d8T62pDZt2KqBaC6gXVttadbPH6-sFOi2AIqSvRADzrPNMi6lL7Ez2ib7ZQBKIEYMb_3bgA2sVhJFNggSCcy_NGKeydC6WtazI1vUa9NX46v7_8JbVlqMPnvCdexwhHmwgrl3oxlMYga-_38lkMyZS8QapQYiomyHFXEVH2wFlh7z1rEqC_JzHItqlw4XU8uD8hsXMfQXfVOiW5lPSX5OzMQmsemFY7WrZklOgUFLVNt7OM5RQ01pDiwpDMrcgfsjCyM0Odc2SrJcY2YHLA6TmDoFFYfC_qK5GbVEKTQR2UHdIRy-elhWrGA2QO-WJVnHXVNd-6SJDrTc3wy3UqnwnxRwwH_TzruyJ-v7ln-kSU5RrBtd0UW7pjjdDAUqmOFnA6N5u2aubOQlP_-lcyZ5Rm3vd_FbKqnnUPmA1-I2Vc3O1yYWMBjh1q7Hp0nY49MZCzieM2PS7jzoNoNg2pJ855JZAjCrVLmUNeCdaActBXV6PLRaf34dxFNOLehAv_n-CTXFslcijqAxlJUWuHuiFmg9UgbJQBMZly90NjfxNEjWgWYdi5vRvgtMW_7tZP_oxP_a9UvjSxy4ddJaqSrGrpcINh_eCuuE1Qol-SRkunBuPELuFor7K65m-s5l33C_CubpWxZokKwyrkd3b52RecVHtSa6Bc8_mj3izj-Mn049rkJS5-o4WjOSNxF7Ppz-Wv-InMxGOqLqa15N_zdF98N5xXuQOf7unH_pNNF-ByeA0rtjnLn1GOXsJy0laI16IYKZf2YVcKcYBP1fTh48hASDt1moUHGPUD05_TapRKHAw9dyaVn_5IVuVmy6aczxgmKc9zr6zJ8N_FjxWKj3bSUT-9z2OUBEtuRhTlGp3xtGEcG27NuSlQiLmbjgIFBfjLSX1kxWuJGinivjOQixABmwvN-lijAvKvLhvjWE4e4yxn-Voym6PS6w7jnjoUYgv-ChBh54kpz1HaFatPz6YEaXTQchFk2pt9uoL0GQOhunbkfr17fYSZ7fhzMu6dBVTeH4is9L90Z8x2VCSq0VKYFYxc-t8PGvayAj-o6mctDDe-vZA2ugMrt7zB5Ok9W7clAW6WjxUoo0hy_ZmIhAkIi9Bw_1d_W6NqeqrIDFLjDkJvXjJQwoGDcUZWLHG68HFKSsaC44_HENr6ICuYoZx8wEdXXpavjNPp7_gAKnfF_gYTUrkDznVOCqpO3dPkVkxduOOeUnsay1cI3NbjTwwl_deDHvMBq_OFEPfdRPiVcSv6V5F8uFzvWDbdX3yA5S6GfA4bWcWzkvSpvc6tOCLgnCdrnqnB_UqT1M2OqYoQyT3QfsVdaLSQTNlAc4u-62XO5R5JkTmlHBI6oRyZAB5AK2gnjF5NLo_h7Gi4qcR51Ju4vKR_BInc4",
            "signature": "TcwoO9oi4AqzfHOqRriXDzjYt-XPsWpC_-AGwKDNuegrJDRvG6VIpLa5GgfDGeqqD-k7kE9Oeq-JBHHquDxaAA",
            "suite": "fmv1",
            "v": 1,
            "wrapped": "yONslmc_2r0kflSf5C9BO3aVCSEo2dzfccIKsPdBvOH2x3gRV6VdSWCpp7ySWeXLMCxuYkJVULQah3Jj",
            "x25519": "h1G-m0ZqMoa1lVXi_OwqetlJdlZMpoxG8Mr2k2TbeHY"
        },
        "write": {
            "mlkem768": "u2hzMRD6I50nJcLUIowUdYJgoaybwRSVumkhmHejce1qveB3DCSaCAo92kBkvwlXFOVC6W9NDBD7oEDREao3klqPQYliyR6_neKHFqWHkGZGUOlZt8yVghZaSWl8RFFZtp6WWhUGUnuxGc36azs0gkH0ivPV_3oIjJpZig9cl7YYvoKBGf15C-4RQfo4CuQilSvNLlhnEHxdc2g6VR2rmWAF2As16uOxVNQQ_gaG7aPyYDE3IUD-otO16MjSrexipKsiEP7d8Q0Ti4TeWbVFR7jeG0Ngkn6P7vsPP19_rsanYKvixDkNV8segjFmnuGkJGnraS-ZVtEV17L6seDQcD7rA0NfmoSIqwwCGxCJ3Ye-t00xrQ7skHeSSKK2G0s2pHHyZfCPej55KpKo9kB-r3xYZl9FOTH_5mCDJ06I2jZHsvXPxZFjJFrgW4G0NS6oiIuU1cPBZlreiVhdqR0RNAeRviR26NgDExufKFaa9k5jUJuQi5f_cTO4tRQDvhhHD5trTrPou20Na_sk63_BEDA60eORkcBBLSLWBIIGj79aNDPQi81IP16fJ4QLfPEKtbvR6TXJOPqpD1y5CRPVeT_4JcWnRUp32n1QcI67uFt658mkeEDWwceuSArK9qMhPhGkXT1hcVeeSu_u2tygbsKZjEmwEcUrNIKLCQLP-xYlMPeoElV8hjVVmn8zsriBkmUoiz4XPBQMOue4tS3d3JLcbrlpE1bAWyp-JN_frSi3uZ_3p9C5Z7SpbGmO5qwRDoqnjCgW2btvgDte9r29J6c1QGvWCH9hRE-jwQ_7ysHcss0q_5lHwfA69fvhkELPkEdk9mlqmDV0Zcuhi0csN-nU5dV8ddZnVjKrBp06xto2DSkvsnIYVyIWlDYVpN8_ufCXmdFwlPKM0KO85hH6kuss_Dd97li7jfFXTuHPKYN-q8X7KLoUbVlC_96jG_IEd6nXvhYPJu4t9-6qJ4oKU4STs2lusAqL4sSJokCU1iw3sPtMxZVTl_MFHGc_9FQHzx9c4HBRDUBpNMwyicOCUt97BXeQCphFK_-jYJIAHRLuD0pZmOmWRTg-dk4oEW_UyvaSlvi-DzeAtS4K0-PpmE7aEkVWnqNFtiMqkB_s2NDKIkGVoTEtyWYEIYJXiQUhS3U59E18t30-xnpKvERZf5YJNajMpXdU-V1LS-ISNw67vraGwq-r4ssu2rtQwPOXEB6vmcnv-jMDL1CPv-JHyxYzP6Kn2rLTrp8KhVuOIV2f4_7Uyoj4eQxvxGn_KhybPJlvr3RfeHw1JV2z0RVl07rCARqJ2Pt7Wk3ITumnPSc3CxBcrTrNKtp9B4nnyO8wXHzhA-wPTYhSYlsj-sHJRZOenjAv-zUuJ48Gq5abvl6nx8murVjcwaZVafp10Ew2WDeERwXLsBLRE0rnWKOLCAGXb3iOGHdjyKTtG3eHBV4",
            "signature": "yu06awU-4BoPl2bbkv8mqPxP_OXOS-Y_tEKDv5DGHRy7VUkyssheiKMzrLGwBGfW-0hiqLmn4P1yVoIW4BkLBw",
            "suite": "fmv1",
            "v": 1,
            "wrapped": "O4PLc2x0IRexp5_1V2wHWDiXsnLLwtHDWLts0SgruVxOzD0XVq7d-3HAnqZPKz9CHDa6xm1GZBisJlRO",
            "x25519": "GfmQtkQo1eKZjz6iwVdkNrRQ9vDyW3AJkud8oCfLQCs"
        }
    },
    "item": {
        "body": "jwxjc6wDN1zoVa946pHjSw1azoz26SkowoLdieTCRopZfZXSInJHycqYIcgfXAy15wPjILsS190hjLdy-hw1dGHzYnKo_ZeohSrTiEM6VphgGdb5fdywHJ4FSt6buFqzBQy-fSxpin-NXvCONRKP3BjPWI_as5gEJYys-Eup0EZYrNsfWjLJcd9r7ag9fWHA-KGxW7wtAVhSrlUU64qX2OnMtg9VPbPpZXKeApUlL6gAVWjOVJ0CEhCIx2TL3XZDqjA7gMvzBAJ9miUiibHeUe4UZhR70HN8Dne9ps8r155vGlxZu5LCoYWZnvwQgmmHTeCrCWYmwK2zRPqJ17EfRkLttzMWs6t91AUfO3O6O9dS2Y2ZDL8ofhElCuU",
        "fields": {
            "notes": "line one\nline two",
            "password": "päss \"quoted\" \\ wörd",
            "title": "Registrar — prod",
            "url": "https://registrar.example/login",
            "username": "ops@acme.test"
        },
        "id": "ca7d25efde78ae687152a00064cf0650",
        "key": "489a11c17636379648cd89cdada54866373df0d0de4445ba10cf77d338d875b9",
        "wrappedKey": "85Q387KvjUNnmiXis9qxapK_EMOdXmxxnKpTRgWZ2m36N9jusA-Dkw_rUP4r6C7TuM-eN0AJ4QCvJolf"
    },
    "kdf": {
        "algorithm": "argon2id",
        "memoryKiB": 19456,
        "parallelism": 1,
        "passes": 2,
        "salt": "gXOAAV3kf3efY4vWoX_BAQ",
        "version": 19
    },
    "madeBy": "cpp",
    "masterKey": "7b05a2485c6bef463b933a3f389e2658311c88911adb0004ea5ad45bb0342b9f",
    "password": "correct horse battery staple Å",
    "recoveryCode": "71CQ-NZ3W-B0C1-J9TT-A8BM-2RX9-P97R-A9AM-1PS5-WXH9-VD43-5V8Y-W6BG",
    "recoverySecret": "38597afc7c581819275a52174163a9b24f8525540db25e7629db4832ed1ee197",
    "suite": "fmv1",
    "trust": {
        "blob": "MCm1fh2cyubV-qyEAqvMG4vK16bZH1jC50BDZ-y8EpMlc-Pst1U7wv5Sg_KKZGWGAbqVXbLerkpzl-DbRiQDzRxOUGpZf_r-S3DcAbFi0p60ZRGvKicIEVL4A3u5nnm7v3hyTq_Yd2TPnxo7L-q5euEkAHP4jqGfRA3bw-76",
        "value": {
            "pins": {
                "bob": "b61b 114b 309b 8c63 36e8 dfd7 7aeb e325 b600 8cb9 2875 07a3 9e07 fe70 f25e 3eb4"
            }
        }
    },
    "warning": "Test keys only. Nothing here protects anything.",
    "wrappedKeyset": "a90KxCdVer8-t1nfYPX2YZ_M79qpxoXRmZPeIupujm75hbUTm6y8538F1LZkbHzfHmuXa8kX-LalBYy-Ky0Th7-uPixau46utplbjOlXmTLdx619JjE6mLvHwQOqBuwlWpnUW-tfUPgR1NRHaYajo1HkA2BV7GqpR5Rz1d6GoOp0Qep6_qfhAw6vTRVu9ia_19vaqbwwVfkSRyxuBpBTGEC8_XvZLmuizBiYtdmNYos2U45z2eyhYAE8nGiB8THhiCEGW5YopIKvKgX5EnjS_iQ-mO8UlwQyT_P5-Iu-66T0ABF_6f78jmURVgsc1nS78eYSMYigjbmEJTN8wEa-4WZa43qPMYdlyGwW3XxmVDC-73VXHu_a3plb69eq9Z-oq1qrQ6DekkFgDa3r2d4CtQ",
    "wrappedRecovery": "-w541sS2Q4G-NDNqjPdzb9yLFsW9QpraXob4YLl-RCJMADiuPjzrQKjbH_Irr0yYHlwDtcRXQ_8zIN-Z_6krZiUSQQIIa1aQt-ocmaDuBRbEgxl-fauzzW86JJfxr-gTaGLtkDehhmdVZoMysW75bUahQI4O1kVCu1T4_-_OGNoEtpER5-Nn5BsKWT6CoNUpr46mnT1ftkY650LCzkSuX00A_vkC16Sk4Mv3qe6E7Boxiye7CIVkXfSrJUuXG_bzGWUldMANAIJp0u1ze00T2VLPh9j0NnG0uH6YLXAjBsYVEZwP45nsE-GgTd_SLKlQqnpg8pf8Nzt-6vlsZOGd0cZvwlEKtb3BRcVIXc2nzN1Tfx0G-ShmJXOGRGTZc-6_9uVqV-Gt-MafpX54NYbpuw"
}
"""#

    /// interop-from-dart.json
    static let fromDart = #"""
{
  "format": "forkmesh-vault-interop-v1",
  "suite": "fmv1",
  "madeBy": "dart",
  "warning": "Test keys only. Nothing here protects anything.",
  "password": "correct horse battery staple Å",
  "kdf": {
    "algorithm": "argon2id",
    "version": 19,
    "memoryKiB": 19456,
    "passes": 2,
    "parallelism": 1,
    "salt": "quqcfVjfPSJMobXCgAao5g"
  },
  "masterKey": "72f7a1d807d76085317a5ad99bd41e19edd06056daf492bfdc442281693748d9",
  "accounts": {
    "alice": {
      "keyset": {
        "v": 1,
        "vaultKey": "Yzo7440Me-OyqDDrgG9dWVsJKYlWTjpJx9gFWtH6oD4",
        "x25519": "0vNraPXmEWvmYZ77ImNd7EZH1QBvaNCNLZeX18MDuFs",
        "mlkemSeed": "g8ni38gnRo8iCfyk165RTqdhNc6UI2aRSSxh64r7aT0JFhDZSCVuaDK_LHyA7lnpQiGsfoEmxmDZ7XO6-m4x3g",
        "ed25519": "TazOCZtKkQs4aq8LTUrIjOTgr3fbd-Ky_9G8mNm56dc"
      },
      "bundle": {
        "v": 1,
        "suite": "fmv1",
        "account": "alice",
        "x25519": "TvbVUJSDB29oMQeHgFKsXwXQBYVD7nwZ_w4TXWaQGzU",
        "mlkem768": "qAVCm7AUiTtT0ZR3mUWKZtxZ1Wyq5XhbBHuAGCUoIZqwJYZkrBMD_0i6v1i-Z2R6qHwknvB3DdSG_rsRWqWYUNgNc5ESxhBGs9q6OzUX6sy_UUscIPqqxCc6JToDBbQ87ikp3OsXWCGeLixS62Sl9_YxhPQJ2aFE7mKAnkB6Udibp0K2HSAsFVKQccE5Hkx0LEuLnVqnTUBLXhJ1gtIA3JZx4JcMZRYLQKwuEysCKFM-0EW-8cUjhIxCuqKgIMHOW7MhAJUrswbD-bHDFjMWQGULv7gtjAGkRXe9kAmYRocHMKiKjSuvIRo5t1uO3ocDXuA3DVdBSBgHPRfF-6eFUFO6WyxW9ecGLyGoNVPE3GCLcgEASXTCyxOEwOnOrpNq6YtFqGUMN_t76FZR8Hsli5Jw_qKrIZo2NZciebYRtekDRRRfasa6hIUHZRvEsAZO8WjOl9lnEqPCy-GuThiN4-lQNWlsQxuw9smwGGeiOoJjH3hP0KhohubK2yQREmsU5UvDsaeBAaSg8LVTYegyzZoN2Ry30jF48PiUVmykORxBr6tLukBtPkNEqwi61wg_-HtdKZZUiSsg_5C5uzBSVCly7PIGbMrFbyOJM6uJPbFmXlCI3-aYmVGvVonGpXe2kaO51gprvIhHO0e_sKYN-vyOgtWGETW537lxQhW3PxOtu4NmiExKMAp-n3PPKXN8bmOSQzZkKic1qdobUlVuX3PF-Pi4I4SIXCQWlyk9Xzq6LndDVyFiWFOs82Jb-MPBqXqaTPQuaTynVCYU6Nonvft8sqatkvRsogUss1PGNwYLwdIIDEg04ayIFpGCOOMJXps4oKAFGkcBTtaRdNNd6dcQ_oLO9-sk5LLMtdqluFk-2zmuJxO09jM3u2YV06yE36VH3VIht2LLiDzPlKdCdywRifl75KxCkWup5lMe9MMBy_QRAakOCHB7XgekzzVfIsBugBMG64kSrGurPwMuyqIeokB4UfSdP6yzyyJbeERejvzGF9WxlxdgL7a7CTEcjIR4SXIrrSYGHNF7PyJ6IsJMt5W8RExJLOt4pnAoB3CG7qCdawhjmPqCd4aZDqpZQliY-5kUMwpN6WEfOrbAkcRILfaQeoS_1tBZrdRB7hQfX6NAvttn4fwjymnG9lpd-VTPYEAiuBtP_4d90gKOUEZenJGOt_aQXtm8HcNhcUyu1lICeuzGNJCLbLtAD_qm_QjCWXhF3Ep5Vwt2emA7aTInMGE-12M4aIdBFde3bMTAq6oYp2cfI1FUw6iNS_yZ6vad6gMFr9glDoN_NTKp-dVsOghQmSqIGrlhBBCFV9B8bjzEtWfOfUkxBwIBLAZdRsydXxd688vBNcFbjfCX-uhXO2RzXRVV-8iYDMACYkPJJzccytZ9N4VOlojAMACsPlUzGSNYJSYujBpvKXLPKJxoPNmh9ty5IAhf6Vdvh0hmx8cXRCsFeXGQaSNiVlnCkXLDGNAX-MK35DwBbmpqkXfF0pmYfyZwyUshtbDDYTCHXqWrwcYhiUc1wugh5fRAnRlGyQrx1cQkpN8yhPv67Hk87eYmPo6SbXrX9fa27kw",
        "ed25519": "-GJ7McujSG1syT00M4O7StbHrH6ozP7HEURHvqDwttU",
        "signature": "DkomUN6cqXS1qof8kSotehEPr2jar6z9LIe5tRVIA-ztsfwT18STsK8-l9au7D1Vhl2ZEguafr735AMnK6tMDA"
      },
      "fingerprint": "b877 3453 6af7 c8e5 3e13 060f 0c31 34c2 f2f4 acfa 42f6 29fe 3c96 8d69 f77e 189b"
    },
    "bob": {
      "keyset": {
        "v": 1,
        "vaultKey": "t-0J9vzs8sfkM2mEussY4Vn5FzqCruwfPaimfIK3atg",
        "x25519": "Ft1XPWV71WzK5mR_FK2fIXqHsEx7QBrFXe1JCs10rr0",
        "mlkemSeed": "OmKOuSaYQYbCgkbCIEqY4vApM8rA4mqefV7rLoRwVKXksRpLY6jmCqP0yIY6BMzXU1yKia9Nj8Fb5hSkT6_5ag",
        "ed25519": "LF5pprLgo-umC1dSKWaHob2UPUWfRtYAW42UYA_x7oA"
      },
      "bundle": {
        "v": 1,
        "suite": "fmv1",
        "account": "bob",
        "x25519": "V2DVmVOcxU77L1NtaqDCB-inGef0DPF1zuJCEnu2wzw",
        "mlkem768": "2XcRTutEYdwPQnyfnBMhsByYbBFbs4hFoKfCKgZzAydvV0uB24HP2ZtRimAesaGEM-YX7xl3fAzPNmaCpIsQ59dWNSCTOsGvM3Esm1QMownLUngabva4AiRMH7u2WNigONajWQPN4fnMIvoO7VcQX1zPVREZA3XHcQeiK3RbuiE0EkkUREc30_GyuYh8FQu0Txt1sYSL1DIxjVPNQcYy9qJmY7lv28XI-9EoKfx7iRTCkxk9NIKt0xpyeTBwwHZVjgQO_wATNiMaoThm5DNTfABHjDWXFAG-0zBaJiqdBQGIMOXPGzsvZgOK5vZ-GiUA9lW8HhnOyCmD2Qiem4V_-8E5f9JtTQx6otODKAqothLMuLOjUElgqVsYypYdrSt1NAM97TfKbkwwFjy6bLscXBh_iYEdnfp0mkWnTVR-CByRIqEpKpIwS-cv47ZOjsIASjB5xzqIHVSEC7k8k-N4PgyvwNGgggN34hoO5GqHzCmJbKkECkySytIxMmnAn4yQGPkL_vWXE1U0ifUfixaZwaNhIcnGPYV9OMMjxBhPH8A-i_OQAUUnOLOKBxfOlKJxD5ue-5hXxnyN-uRtbwI-qZq5BxoL0hEM5UGprysvfyKIoLiZFqplFhKxeuduYAkIwto_-KkniTMGYVGmMAWScPZ2ZfFRP9WLaNF-v_ts_IR8Gwqsj8Olk8AO2wZ6msMfdZNwADpe8Nc_m_tMT7ZpkAaYzkqkAcIOzabJgwqB5MhqzYsUqxItmVC09mcCyOFLaehWsIQm9ONHTABbY1HGzBxK_jltwQZFojBrfVKE27F_0amJ4THKGPmRkkgzYRZOzLo3lYhlYqiKKWuCgaaTvbMm0DNA5gon9JiODlmxJHK4V-p03LgmY1wHONoDYKMZ9nKHM6CE0aeAnIGuxUyrnbc6Maa3eSoaO9MDXFaG52IvAJG2ccsu3IeJQQteVdGaxBGDVEEaMzNHWIyq5bQ2RHclCBaiCiwLOfweKaOovgQT4wA89ytFySKH9zR4jmVKwBM1EANHW0OVL1lPsqsSXJOXZ9m0i_q5OOpuJZFr5bmAdSZJs4Mi_5INbwk7kfJxCQlVzbqjvOZpvFPL8RHCwAdnqpY9WEFJhuWB2XuWAkXEM4awzPOJhyd3ZDqzvbWLXMNht-kkRXYhMQAYfZashhNY2HMljpTOa9JEykk65JG_pFp2WjCPLOusqkZWotZLn6papuhHyBpRmDOzIUMClKJsLfMyuIQfgZplh-s2-moP1YVFaLhjj1u5XYDFV7GDGYNwyds9ECqshCZtb1RiTgJsKCBnv1sH63Ao7fo0PeQ1OeVE1hI52BuGBDBvKQCm_3ZyFIsyEnuLUTcxiFaM4VxuNCdjNUgaEamdmvGsEvi90COtArV8spN9VXFAMtijkbkO_ZhundBH3DnFbvCPWSId0-Uh5lRXBPdeyzAyaidBbDKyXuuUMut1CiMeZmQ-F6HNGGR6oTQZ52Ag31FcyXZlETGVeMYi9YZJEZMA79mlbRIcXGV0BKVVQHaQqzUV8Hcl0fm-TR8sE8B5j2YTHSKBPci-LCYLCeeKTXesrzc",
        "ed25519": "0V9mHiwe4ClFh6TuKqSlloNCkybfJOQ8Yn_76kEpwAk",
        "signature": "a1A5J-vN9iP_j2h-IMNCZHYMtWEcc6YY00BKr79qe0KrXgjjW2bAWGjwBsEdXdK6qw2iR7pD93jOTbRxmMwSBw"
      },
      "fingerprint": "8f23 92a0 9783 a30d 1e96 beb2 a2a6 2315 d7fc 0e97 5d27 b2a6 085b 8354 9cbb 5411"
    }
  },
  "wrappedKeyset": "B7cTrp1QGtL1ZcORPD7VaK1kHQ7aaS_j6XdXkn8RstC7pDPV0t9DfGi2Z73SDLgcxWziPaTV2ff_KewV0gKgkY9zttK_nWd7OBz-PuvmO603oYNpdq-HAxXeQCKAx-LFHsJAUjeFHaxYQVQHer8TUKvHGSw0QOMSAXFwZ1kLeYdM8K9svqGAsLr5I_r-wtKdruwY3Wzjra32pYrIAIAaJHpvur_6vEjvdgyLY6hY8c3avNhNt1wQSRm4nHyPJ2g0cetJvpH7sqSxIxrjQmMeyUYSAm7fR8YKon1XLZL1ydnoAT5zPFJZG9tnINgerdjw9ptWfUn4Rn17q3doDNZ-eMZjpRaOnHrYnK1xwKLUEbT3Pw8Jf4DyYcqn6FmtLFDtlleddR-WOd8_UTwICGYzcw",
  "recoveryCode": "VVZG-CFYH-4T63-01SA-JVYX-B1S7-4HN9-N8KQ-686A-GXVV-XTWR-B1Y5-8M90",
  "recoverySecret": "deff063fd1268c30072a96fdd58727246a9aa277320ca8777beeb98587c54512",
  "wrappedRecovery": "K3M6Wz4lUHe3X-ZTMbDiArV6xSIvW-cOiTvdg3Hj5baVsu9FIW1c9jR60tzeIT48A62rfQ97MJOtVLmtIM1jpZbO6ofsKaeef_cY7lIGvDrxEwoby-DYeQoNDrHsj0CJFlfRdW-_tQxDOdShmfL3LMthQdALDPPOorQSN1lxnU9OtJG1PWPLjZ9pZsn3Bo3UCrHZI25WauA4c2irJ00HnS53rZI2u9qKTVtHepiPkcXKNwSi8T_6mdr8vz2gz8y1DvdSal5NPq-k2IWP2ZtvAYcxYUA_aYEBm-MCdjgw7524uZPCjKbnG-DqUJP9UFG7E7z63rZniiDxzziWWJmOo86vOQd6iP-O99Vl6JrQVI_6C6q4ZPfBK8cOXHZeAdmay8wzuASZy719f2cJ6LP34g",
  "item": {
    "id": "231c84fef2141c7d7777f65d1fdaee93",
    "key": "8195dd05757691fee3e2753f55f1a568a3a9baefb3414f09442af3d9754dd6df",
    "fields": {
      "title": "Registrar — prod",
      "username": "ops@acme.test",
      "password": "päss \"quoted\" \\ wörd",
      "url": "https://registrar.example/login",
      "notes": "line one\nline two"
    },
    "body": "fBpcuK00nUG3i_MWqRIJnlXOg_MmzW-zYcg3nq5OP2OxqT-Q34oIP-fKIXWHsPKDS-C3Yui3vZ1-4N3Z-fRW3-8vyozFBwEYc39kNfOT0-H1QbOQIHfQjJRG3YABBjV8pmJOEu_H0LPPbfnB8cTWZMLdzLsoAv9-_Er7hFmoIx7eVpQDLJpuvlCxq9GxMo9zIQclaIrx-1HkEilU9YGoLHlxsyOmrhNkzyAFtyPe77-XrVHVJnN-wg-OT8GDt-m6D4WZBVDf70pf-QP5oKZno5DcwuS6o8Jvye2vVusrmBtm7IlW1cGcUfipSYLLclG8Nfe1_-Cr_J59voIKVSEiLzb1INxa9U1mAWH784jUy17TtrTa7duNFRWkMug",
    "wrappedKey": "OatpMdVjd3KdeLRb6SJismtWw2xqfrvhqoYPG2jrRXLQXQCZZr-d8bHvilixXGo9VZNE8mr41K6u8ZgO"
  },
  "trust": {
    "value": {
      "pins": {
        "bob": "8f23 92a0 9783 a30d 1e96 beb2 a2a6 2315 d7fc 0e97 5d27 b2a6 085b 8354 9cbb 5411"
      }
    },
    "blob": "YBQII0lDGCiJOzrFoitjRfrqm12AWtOMe8rMzUCcqNQYO5if47bffFWRd5GHB1ya7MhPwLqkQlQr5vmQXy6KA4nOA36eX7xSJHOljfaCNJjM05R7IPhH9ZodVBh5H_cxD8x_5K7t2QIqYwR2MDqWf_I_zNBMVqzFqUHFURY6"
  },
  "grants": {
    "read": {
      "v": 1,
      "suite": "fmv1",
      "x25519": "jBy8qLeNiSp6X6tbxxaJzGhE3NWm99Y8iRzA0F7qo1U",
      "mlkem768": "F2N7HfKkphsnLQuyiuGBz-8L4lOoR46P_tiP7VNraRm24ybMDk8oJCUFmhupP0mpTo3-lQNqKlRq6J6_4Uiunj-qfwChS4E5oqBfbSGMY3SDanK16N7-KKkr0EX6imy1iTBspRsubxktIIF5K6hDiPcJTSy0_XO1VHHmKrcNk5h0AWKwuF6mG9vZUBhgb1lnyqbSV4Gl7oG7QJ4H9MCXuydNGzfSo5re0GU77uogcAUE6PiiRX0OkvbOgRGh2138UXdJ0LG0D0ShHJUu8UhsJ0I_n3nHvJIj0dA72JlUw_dS_LRy3ZNBj3ttNdtmiJmHlp6278xdKsWYR1pK4JswzMfbPzuXKeJaLV8V_qr2NcFdmu0TwlbdaELoiALGxsW_TXoBPRmZm0JgPq4OQflgmoV0-w-YKeBQ16Q0hmkp-MR7739kiemXHdWsJVq-bSeINBj8yOZvD5jVl8ZvTv_AT8oVWW4ns4QQg93IJD6TBpud09sEQFD5q3raEUTl4KLiWU6Dk39dD2zP2GCKgNX-7pS01pBprudTi0k82gGiPMJRdld1lrfCPq12POukcA7cHHxSWuTZIFHrHLonuQHm8O_UzwbydJn_GVjt3vea1gSxBeawoqDYEW2uNenpPajLBcIZkjMLJ5d53V932wG7X0wu-4HvR6S3JKNnFG5Xr7wKZHnctYPTkjb2crd7TU6CM6lEAi7QHs4mWYnBAxfyxesAXbBHN5ZWZ79OMhzc6snVmfCXzOnE_axf7PmnYgSaylV0-TqcN8KODKGl0JWaVb4ctJLvNSjITiedPZ7b9PqGd-uEJM4kjUpH62ZftzNpvPxbBhN1pbBbzU6CrguOYhGS4nuiSlm7z-kAtj9eWo28zHpvM6lpAybNeL4cIhdxhbCBShdiskOwMznFokvVxO8MeLjx8UEtyOs6phPenPpYb-y7quRYEKe_nic2jafAIAmWO3jTC3YsOzNfneto04Dwqz8JLt2LfqSIewoItCzcyfI7Pavvo8fJeAWJtenJ9VLuzSZBTGgjCaaRdsfhf15XEftKyRU9xR9NBPQeUV2_DLOT6mjFCO2nG2ymu9IFu8RF9eO-MgDcvuHhRit1DmYti_FxKgWHXLTS0iJfFIQZbgEHAqZ3lKY4DsmDLaMUw4p5mn5heKVejY-CZJspZhe_7WDgHK6kpenB5fvZWBcSf0zSnrel5NISekM5f4Ls_VJYd6Mqyv2er51fFf6DKdPNYwdvvqFRF4GhErTgF4iU6A2dxjEX16jtKdD1PHjO2ET_jHbKvQDcSFqU2Olcx6vOKZRsyl7p5tHpbJmah_RSEFJzrZkSE-5kb5ubakKJGcjiBDXWp2S2AGIb1-3cvVnGY8HSfFOXfvEDG8lNuWpv78f_fvBnxQZaKj8mP05JdxRLOHqsHNv4HEagkPDGEH8g2zWffNYnhK3bMvs3A28",
      "wrapped": "3QgMUfU9rgB7-FwWuWrRA3bieKK-c_JH__EKwAoPKEFBTlk5j_81SWcvRJQPmU-YJ2NATWZ2e1BOXDQK",
      "signature": "s-5nT-v0XuMV6mE3it1JCwPmXj199b20BZfpE7MU9aubj_acb03j9x-AYtwdC_33rw2Th43_AmGe18AfezDSAA"
    },
    "write": {
      "v": 1,
      "suite": "fmv1",
      "x25519": "0Wv2Og-t5g5ycYwYuflR-Bx8Cqc6qI1EcNGkqDLyuTw",
      "mlkem768": "E4j3HwcOvI8Z0AAysMkgQUbi-EJXj5yj0QPVNH7odDwqe0FBoHDqhYkXxIpq_05btYBwvW5Yc7JkvYhz32q5PdfC-f6rs6fteTTfKa5vaM1O3dVWbFHqz-WeE1msLkqA1qJ5WHYxBPKxoCbJA80A8j-nLYs8f5ggBMMlLKxeD1LmpDZEMGF5qShFjRzmDa4qArxoHXOyBTz1K20AcFs84IrT5omycTdsid1-Rih4wUVKyAph0PkP6IiV0Rnq1qa0Qt731n5SROR43_Dx0eGIx40Qq72PP26pu2zvx9RdGszEqMTgPWe3-w8tC66uKKv5smdHcx26sDRQE7d4uugPabAeTSZNCS-DUEBS1ImMZZLMmLyP5f2T0xRtqwx4Lx8CT6qcu9Mt5Itqo9FHcYN8-aDQ_Daq-oF0c9TLFgF59474WNKYfmBgwcDfxzqQZiYVZYXQ4pGCefshRKqZORNIop9UXnNMG5BSqOUfs486R7U7QP3K4Dif5S3Uv5LCLg0X7Mb9kIgZqiMlXWViqDqpBUtjqP8xcvrHG6cCDM1TtADEMs-a6MlfhSoRJ0iKsJs-VmnOi5iKsjerUPYvE0GsLfyErgpDz5OfGO_AxNbOoX9wVT9uhiiuWnhMQyZUryCsNH9LQhe5cjg1RjyftVT2AYm3-c__t3yYeTFM6twSt5dC6Or9ZGE0JewnmAOGfva9dVC7XhDF-TZ-mvnI61iA3M1fW-poncnf6jS0D2aTZzpK_sHTTi8WnCyTM-kwIjWt8JexGjjHaKIPd7QU3arKEtR8LXGnADKWfcIYQ-v5vIMVLG0-OW1XoW4eIGxK0eawx0bbjzhPXy-t1KIrVWoXhYpg_l2E4p4MP9EbMa-piNRZvAY1xVknD2bwYLjcN93JyWC5gYyWkHC_0caY1owajS8-gPgAe0p_oP8ual9YNb45RwoXLYtf-lqNx3N-NyjDJtXClA4zZ_GoiCiQzSCT7YJpr1kavN09hqO4Xw0ANN08ZL7y7U9KVCRH_z0GpusYJ4yH6uj55RMnpLeTu0L32qQSLexEdAVlAkxomQ4ex-qvmTPrpLykFXMyc73VqBLpuG8Da-jta72Udd4Q49hWyaS-bd_3hOd676Y84quTeZ2rDXL3Waj2f0fy-ntRAwkPM8fxs_ivIJqPQ7-ou1OLLALpQmBM42OJ-hXh6P6EB9O-OUAwwEoAeUFi8iN9nr6b9QAW3RTRF3B8cvS0islfeV_0CasGD2GzSbYaWonfEbia9_ydwrdD8-jCSTh3F2WPcoiAXN_tm9emnKJBQyusJCK2z0HmAf48CxQvHH4BCLrDCaNVJvGTCOtS5Q6V89CkLujD6w9G5AlDChEvxCoex6F0uC4sJqUhQDE9dWI-s19t0af-i8ATb2qFY5xiRHvXcFBlFgzuko1GGQKdIyo6Bmrvlpp2NLMjWztJ9O7zRWY",
      "wrapped": "1yeBE04-3l1Zao60-1UaaW0yD6SqTzNhhVZTztOB17JU3jbMTeLF-cfWC2ACgECNlxuOfKaizPa6mRwy",
      "signature": "13Q5VGqJujmenY_o-kZkgCWLwFmqovttCpGCn7rGRVIgdeTpXMwG2O2bKF2IXSnbVs7cEECE6E0SOpATjISMCQ"
    }
  }
}
"""#

    /// openssl-vectors.json
    static let openssl = #"""
{
  "source": "OpenSSL 3.5.7 9 Jun 2026 (Library: OpenSSL 3.5.7 9 Jun 2026)",
  "argon2id": [
    {
      "password": "correct horse battery staple 0 é",
      "salt": "f947459bfe59a7563b64eb25b6de9499",
      "memoryKiB": 19456,
      "passes": 2,
      "parallelism": 1,
      "length": 32,
      "key": "aacdbd758701ffe0deeb496ca2bd60e396d58a9894fa8c95376c13abe3826bed"
    },
    {
      "password": "correct horse battery staple 1 é",
      "salt": "60ef81db78636ced8648ca61d4894724",
      "memoryKiB": 65536,
      "passes": 3,
      "parallelism": 1,
      "length": 32,
      "key": "72a47f9fa3fffd6feaabbcff24aa66c1962a8cf3aa5710075702a0134db2e8fd"
    },
    {
      "password": "correct horse battery staple 2 é",
      "salt": "6a2106d68c48d9ba027d0db1ea2f1327",
      "memoryKiB": 1024,
      "passes": 3,
      "parallelism": 4,
      "length": 64,
      "key": "e11cae3d803ac5ee6628f2af95589073225489804a90e3bbe87700348117bc9fc3c2510cfd1a162200e96298d768a0e379cf8f432d97705225dde1c06eb0df1d"
    },
    {
      "password": "correct horse battery staple 3 é",
      "salt": "c6e4222f91aeab3218fe27a3fb368d7c",
      "memoryKiB": 99,
      "passes": 2,
      "parallelism": 3,
      "length": 100,
      "key": "bc42417d06107d11069ba4c70e9cde20ffda044564c8acccf53cde71f5d86eb67218dbbf1f8599fe4f36450ba16404bb700e3906bf2a89ef147abdc373aca1a70f56827f2fbe398c58524a79d84577fe28f787fd40a213e6fc0b90b5c53c34938d182503"
    }
  ],
  "mlkem768": [
    {
      "seed": "d9bd72e490bc181e2b6f7379d01792ca435d8c5de407b3eb1089726084e1fefa7d8a19996a74e056c096c666c333bf126eb08da46f4a57c2653016d8cc71df05",
      "publicKey": "132a24fe2b9c171bad5d46ca1ab4c4001b0f7d586ac47b4d7e5c7fd4c258da5674d7380030708c6ab5a81e23742d0044c57a84920c27fdd20891555058b0caf3d8b582e9453dcb3e6e28972d98a3c5364dc946a8ed1254757a1dbdf4480aba79e9e86e58f37096984ca6d100e08100a369bd554c5a7ff50d8e197a925453e07b4b830b60247130ede18c05db76aa72a09bac300df49286db61a56c1bc5a369aa046739326fc5aac33668b903fa9c98f0657ad5ac21c555b7883274b6097aa6066c0465d4296139ab1c25c07b7b925a21b4b5218506f56c185eb1a0c8bc3053795edd83312788bfec631855004f52f184cd7964a7abae8e9022aa4c29c1a43223a48b640a5c85637a9a417f2a03212f18aaa800bd32e90401a13fbf959c7f816015b89e3804b299210f43d1639079b403b6cd62db597eebbd9fc560316470b052a8bf682ba0868158796113954bd4b564fcd20d7d33cd470a3fd23b9d5d2083df85a6a5b6a6297c7e374c54f68039dc9666d61891edb4b70f22100b70795bfb6de20649738173b5f9236f71060d90487a489ddf769cbd22bdf12b40437a7cbf61aad24a057c496c73e2959616a9ee122dc2c50cdf64c3b199bdf064b481f9b334306cd891729cf7c92c4bc90b7a8da3116c5d9b19345c2100f31eb846a9d777421ba77a70a63955a9923ba00003b4cf2b0c5acbfb0ff91059ea32128d6741fff129d806a0670216f56cb4854302040948437a9fa490cb386667aaf6583154596c732706475fe7a299918c4ab036aff892c65b9c043a980db7749254ec5c0dca5b03f668779185bc48a2c525b86515cc4a1072e5263790c77307d83cc35880f9476425b9c805e44c2508842593c2696a3df026066b4a74d667cd4a5846352595cd3b6bf98c2bd09a29ccc0692b9bc050ab5ca1b00c516256c101bd25277b550719102cb0f56090a8c763e845035643a958d21a877a8c9e484d57f9768ea12d654bae44e7b929f6c5d6c08923e32aa19ac79bb6a44c28b3e42153db16a5875abf5c5510ac02bf3fd8a339f6b236c75f00950b0a84176c25296cf8662c97988097c76ce9a56823a109c43de9276bf2c5c1e144973870a09fa14d1a085a950a03eb0223dc22af901390af12a674420475f339d2364ac1c01c9e11b7ed826078534df6b53e1f6a667b56090cf5bbdfe97ede290a31c69001f6bc42b512d0bb55028c3244fc57123309f421ad44ba25df66860894b88bd118a0085f719163729a3bc48452d7cb8f61214ce7051f3eda41250cbb5f2a8d45acbda1c237409604503a76ff7630637871f841148ad9732ed94e53cbb2003063efa30e6040190ac41e5a2201b1d968d23692fd01c6b3116ecd64c89a651e2e73407311befab2b45dc95c6e279865936053726cad7c8fad773222f40cb664609d766f5f7a791a823fcf446d924994a0f526c967bea2c6535cb363a4a42d34762e1d1803ab750b7a038c000857dfc44350e93a325327960b91e8d617a7444c3561930430cbe1473fc7f6305d4c553dc2969fc97a43089716e968a9d664d161cafab1305036a4d1b0b8aa5c3f26c9282c256d81f02750f32276513bc7d151ee6724a4b0274db14da00b7cdf8761b9a5708cb87faa17da3e4caecbafeaaa1df6a00f6ed0f1cea972d6e576d70650",
      "secretKeySha256": "06695c1901239899a3e0a6bdd2b633582ad742a3d8dfb9cfb41c6df73ba73a87",
      "ciphertext": "3c834864bd407bfc7fbdbd6064e4ac98ae0450fc0c452544dfb3846ac4c7a76ce33e04635db7fb7a4178039173b40c9af03e62dd1f73ecdcca4f136c108f803c5f67e36d1bdc2732dffad68076763af8ac0f0fb75e0d603ee3098c6336efaa06ba1b3d6e48bf63f415c1db0388f5babc315a6e9bdc433e28fc3f24d27bb58c69cb83e9db1dd7c1386763e20ecf864d8c5516f080fbf07326b749328d95e29bd9d005f46b41dc9bd82701ac2c973354d569de38c42d0d88baed6d57bf7a65e215f79593f79cf1e91966759750a15a2b4195cf702d0818997b9f18d4b74828904ea05ef88c746b2ca2020def9e458b139d70f28b6784a0ba9a6c36319ca9f2b668bcb3f4207cbe8dab4812bb3da5e4ff9452bd6596d85b7f2a13ce83a6931678c3f1972a7d3e29d96ca96e854377b407f225bc0e8d2f9fd61e1c4b501e6467cdf5eb8912ddacf7c0c8c100af7e9d80f1d4add2bd8887674795053d10be2083bddf63f6a0d69bad54458475cf41f2bcb8fe774405e23a308114905d9b5768355371a06c1b4ade06cd7bc7897f9b2a4e2e63ab4acadac89e05eb195f458440f2f462e6972a82174b4a42fb26b97e4f9a2ab7e5c736ecad0dbbe5826ace5db05a91e5c8751140e3f177e85dd096794457496e3fd17b5489028f9e545ca3a05920e64b13e92e7f0a4f492f7f2b03e258b10f5fe86591c4de797f748a7beeaf3f2e921eb8f9ac784f3b9638a61b656608bf93716a1a31a65a0f6c51512850a8e1200a6b0ab3cb3e568a382c0b19b2e681caf974caf91a31fc068d9add85a26b6f88170474ddab2876c4a17a943a982f9170f8eedd6c38ba9596931119fa2564248a518b2434bbc7550d73030a7e0c8d055737467927d3837b57ca0ba40bd2d425f15886f0e3d3b1726746b2f7cfbd61e484b1ad74160971177da2dbab27bd29b0a31acc9addba8854844a1b0ad053015ad5c9a4dc9fcefb5e0127d468870d7deb2d117672ebc372f856040a7281b73a341c75e3049ae1c393fede5bd6f57c4cdf61cb0370002828a9454f5e022125c5083d2e1418ffe5626457090db7c1fa1ade1c47b2398a9aa8c386023ee03e720767d32f2c736bbfd44dbde0338dc0ef157728fbc3c947aa78240a235856ccdb39e55b98fbdc72d258616907244e416e1d3a66bb9572b154f4590a2fa9b91c1c3b7b77692a1980e200db731f0453b3481602a41a6839cdb9cb40b982f39bc5eaf4940c51e4c8f5d825ae558dd492db3ab783664ce883548b50b4a8fd1fd13469a6693596355c9bf1715b28e3e0c57f227e6b75c88f19b08c94e4ff5dce0002853aad4c24df72bb3188efdaf723723df6b0d574852e5080c2b1e64eaaeca66c272ea50dc2e1b39f75b96144e75cddba0fba0317d3bae1fcc9f7850f487445fe6b8c4e518c28f9cc01aba47931be2bdb3598f4be43b1f46b81b79d5200c54debb88c0f3cc12ab82af6b18e8c37931a561640f49c358c1c26559c5ae2ac0e7d31b969bdeda3c56a65e9836fe967338d0ff15e8c23fddf",
      "sharedSecret": "ebc2d7b289cbf09577903a0224b08a866ce01b127b00f94104a339165d43c547",
      "tamperedCiphertext": "3c834864bd407bdc7fbdbd6064e4ac98ae0450fc0c452544dfb3846ac4c7a76ce33e04635db7fb7a4178039173b40c9af03e62dd1f73ecdcca4f136c108f803c5f67e36d1bdc2732dffad68076763af8ac0f0fb75e0d603ee3098c6336efaa06ba1b3d6e48bf63f415c1db0388f5babc315a6e9bdc433e28fc3f24d27bb58c69cb83e9db1dd7c1386763e20ecf864d8c5516f080fbf07326b749328d95e29bd9d005f46b41dc9bd82701ac2c973354d569de38c42d0d88baed6d57bf7a65e215f79593f79cf1e91966759750a15a2b4195cf702d0818997b9f18d4b74828904ea05ef88c746b2ca2020def9e458b139d70f28b6784a0ba9a6c36319ca9f2b668bcb3f4207cbe8dab4812bb3da5e4ff9452bd6596d85b7f2a13ce83a6931678c3f1972a7d3e29d96ca96e854377b407f225bc0e8d2f9fd61e1c4b501e6467cdf5eb8912ddacf7c0c8c100af7e9d80f1d4add2bd8887674795053d10be2083bddf63f6a0d69bad54458475cf41f2bcb8fe774405e23a308114905d9b5768355371a06c1b4ade06cd7bc7897f9b2a4e2e63ab4acadac89e05eb195f458440f2f462e6972a82174b4a42fb26b97e4f9a2ab7e5c736ecad0dbbe5826ace5db05a91e5c8751140e3f177e85dd096794457496e3fd17b5489028f9e545ca3a05920e64b13e92e7f0a4f492f7f2b03e258b10f5fe86591c4de797f748a7beeaf3f2e921eb8f9ac784f3b9638a61b656608bf93716a1a31a65a0f6c51512850a8e1200a6b0ab3cb3e568a382c0b19b2e681caf974caf91a31fc068d9add85a26b6f88170474ddab2876c4a17a943a982f9170f8eedd6c38ba9596931119fa2564248a518b2434bbc7550d73030a7e0c8d055737467927d3837b57ca0ba40bd2d425f15886f0e3d3b1726746b2f7cfbd61e484b1ad74160971177da2dbab27bd29b0a31acc9addba8854844a1b0ad053015ad5c9a4dc9fcefb5e0127d468870d7deb2d117672ebc372f856040a7281b73a341c75e3049ae1c393fede5bd6f57c4cdf61cb0370002828a9454f5e022125c5083d2e1418ffe5626457090db7c1fa1ade1c47b2398a9aa8c386023ee03e720767d32f2c736bbfd44dbde0338dc0ef157728fbc3c947aa78240a235856ccdb39e55b98fbdc72d258616907244e416e1d3a66bb9572b154f4590a2fa9b91c1c3b7b77692a1980e200db731f0453b3481602a41a6839cdb9cb40b982f39bc5eaf4940c51e4c8f5d825ae558dd492db3ab783664ce883548b50b4a8fd1fd13469a6693596355c9bf1715b28e3e0c57f227e6b75c88f19b08c94e4ff5dce0002853aad4c24df72bb3188efdaf723723df6b0d574852e5080c2b1e64eaaeca66c272ea50dc2e1b39f75b96144e75cddba0fba0317d3bae1fcc9f7850f487445fe6b8c4e518c28f9cc01aba47931be2bdb3598f4be43b1f46b81b79d5200c54debb88c0f3cc12ab82af6b18e8c37931a561640f49c358c1c26559c5ae2ac0e7d31b969bdeda3c56a65e9836fe967338d0ff15e8c23fddf",
      "rejectionSecret": "08f51d323636b3a9d94c9614901e295d82ce64af8d20630b2ebedb85ba680a2b"
    },
    {
      "seed": "7e9f66320290a341a1b149a153d81a0867f74a838e3626b876534f6269286d989648f4af44299ed506a8c2db8bd3ad81a9fa1db551de3d77cb122a01723d69e5",
      "publicKey": "818b905aa6b7eca89f70e504513130dcd76325151700b151fad1077c179f089530c58b3cc3c32aa198391be4a34375254ffc1727e199f993cd51f9c200d776e792ae90e54c85f1890e173bc739b4fa3ca8ba333d4ea5395ec455e271272cd96bcd4bb29bdb88131511b859a12f603ae34426aee31e29a79aab2072c391195dd6cf9b393908e2c3ad5a40b0604427aa9405cc258b16a9c4a21073aca36f338d90856100b712f4e8c420947ed844c7fa4397dd97aa6df1bba08a37e661ccc031033a7b8c94ba15cc868b4339cdbb32945e84b31237c6ec700cbcb962c92a4abad26307c0b73c5b5f609608d15080d521a1269ca68da06793976d57dc9784a730882184a05525228b1f85400d2432009c58a8a1432bce36bcb4c5211e335b4b16b57ca395f9ca4600c417ffa07d4689942f434fe695a101ba121615c24c546f5d5a08fa0653b0a7c7bc743577c5ac8db7888a562585127652d5586f976d5b3a30533061503a568ee181a9f053c64a83ea5700a2aa0222349756857c0f641637a85b7f1201d9eb0dd8337f04547f5ce8bf554361c3295cebb7ac04eaa6c43970d946085253186ea76b92d07231a697f8abc87e965a81e2bfed4a30e0da22dd9852e8b169669775bb361d43d1b0db21cec247473632cf46161be6422671fa24131973e60c3a32527c6d251d877b72552232b326702eac2a7aa644f34a0da769177062a87607aafd3c6c42f705fdf15ef7b236745b97b870ce5d553ad6ca1491035ed333b45ad4ba983616ea4a71bdfb734695bbe1a1820d46792c75860cb1673fdb97e7790b154cbaa2219853992364e885ad1b5524bba6dc53c0192115eb2bbd8c0887bad7845c1192f0d3951d683df0841d65801fbcd2a4ff64b8e885a0b36656cf79458c599dc1aa34008c0fe16202c6ec291391cc78a1579be1a35a061d56cc9c20876efdc664c323b747a05f1edb24158bc801893148c3a94c980ed9113c52d681339aa03dea6fadda695d9a156a91335b22b918073c9456371201ce1683a919a0a05984732503b035d8b078c57db71738a3f7a1d24a4ab232b8e17a157e23c986792d3ad19f5952ad3219cfe18365779202acd1af5d24a2fb5a97e9590c9ec2bb0dd71f6058a8594b81c87a81d25cb2a8d76e0112c9d12b979490c538ca3705806a987370a96b3471ec14eff6968ada26b922cd61a6b5b9b4088233af69d35249f399b1483036c72cc27365e443103d962124d4caa90cb069e446b5702c08418f4de96c173174b6625ce26902bae400e2ea04c7bc19cc3010b22512ddd8b78a168383b71ff55cb60d7974dc127bd7064f7e43b330a96b1b001d76e87ccf5a9d9d9204b1f541260571b57b865e3c3e3c975023f14dcb99552e88014090631af042e2b533dceb8a71a96ff9343289c4a3227b84ef9437967743cb72473210aa539733c1d4aa55d5088ed92d33e18ff988a2000065592b7eff7c4c578159a50ab75d5854d9b04fa1d89278b6c811b672fef67cbb57aebb3a4655c90655b5b00b53bef2d3b3f808963d796702634af3a40470b0166057aa5351af939137e3ba8e7c2a61654992e1274bd1d229c213805825c3824312a2820e09972c2b6a0b3ec983a47d10f59ab4c170452309e8dde6bc903e9dd0329a189876e9f0eb1ee668e786",
      "secretKeySha256": "1c1ab8669eab31ee5cdd27f7bbf3f296fd23414b04e808e01a2848378c348c4d",
      "ciphertext": "9db90e4b18ba0d3228efcaaffcfe48e15227720b229b68bbc5a1e8c77ff84a34c54f5914a941a75c8f5ce711d27bf4f8ef257717002b97dad75eaf394760d3fac251241fa24ee8c9e76c4bdda86e9bcc6663545c4afda6e4610b26b0f4e1347791f38733f855211dbc338d500399bc9c4360c5ba03dbd95eb029ebbee17dc3e7de8d24ede24c58dd100dc6dcb02835ca80b62dc1e72395c3f6f02fe57f43bda52191ba22c6671ed1f4ec1fb344d66e253b3f1e6a86b0a8891ef859316db955c16f6643dfdc8080e252322711d7da781cdbf374e7f243605fe4bc1c2b99cec53f08cf46e3382d09d20868b258a651bcaa3fac7e8204465ea6a440242a728431c57aa23c669b407a8ea0bf5e9184585053be59d970de18b4d406cc0d6f53c3d4273cbc85b198910269b61fcd0267b0c66273de4579aef4e90a2f9ac5e7ea405bb8d1f31c4de1703f5cced686e707656ce85e2268f2becdaa3a70c9728e3b37f8120fce0d96ae747c0ce5d9587546b391898320a46d07538786a72cded7d1aa78734b0eb348da9d7d1c3a245645af4ddace809a33c80ef777adbe074f0910962753c278f892a0edc0ab7702f32ff45790889d17e83030fb93fc362ce0fd7f0075faf4bc33e59ad18b27090469f10945d8c2d4126c4638faa8d7ff04152aa37956c75fefaf7ff2163fcc3423005f77fddfc1bd57520177f61670ab54d1396cf0163df6f35a56e9ddaa2d69b93985dd7afc542fbb195a87cdc210b4d8cda6803c7916c19a4673ec1e29f1ed6629e4bfb1a6df5199fde8b24099d05d49db8af47fb210d774ac10424387d5822596213fb73fde06a2f02ded69acbc1e436c1cb6881e88d5489ced8b5c689f7883c1b630c237d18e30391844a8a2b039a15cd547aab9986b07c90070bce10317c4743d7ef90f42a80d6404b72743be4bc5e1474cba4430e9f65762d2b87332b9ee93edf43c229cf5e6f8214e28fca64ac49719fe9c2a9bbb5ac1f875c3ebf907b31cd32695310f7cfa5c808e57d1b85f89213a190804c1c9ae59088fb1abc32513731b06ccf4e49661cf108e5e687241967c70a88eb9943fe53301ee0a4b5ec0e54e55b917d085327dbeda5fb082533ea20b6cd87fc504c08af66e8d7a2175263a67f99f54f76b25dd71f884450d17109e91b4bbeab0e9c8cbce0f16422e6bcb04b2c82fc0de3a30232d3573d148010f804e32da7d968a7d6ccadbd8919d2744fc5d5e07096388bccb66e4f820ce544efc27dbf801c28a05123cf8564473972482ddc00b31b68884eae6043aecd6e6bb988994546678aa33a0f8f538cf80459fbff9bb29aa4969c1f0709e306ae4a0fb8b1c7e0d0c678ecabb2ccbed3e91767002ec9d5b59f0f90a8ffd245536c116f04fa4ea8eff55d9f917ef1b91b9cdb5f584a82d84b08f9d6ceff783c5e6205236891b25b0b8063663bd23749e9a6081da26f28bf622a54781e25f60e49cd415d8a8d93d24dc08fa732ac912aae06a550b4f0ec96cf0c96e2edd68da8c5a21828d5dc97cf13b30fe",
      "sharedSecret": "fa419ab60dbc341895d37e311ca3a2cdcb77abd625f48fa63023986763a749af",
      "tamperedCiphertext": "9db90e4b18ba0d1228efcaaffcfe48e15227720b229b68bbc5a1e8c77ff84a34c54f5914a941a75c8f5ce711d27bf4f8ef257717002b97dad75eaf394760d3fac251241fa24ee8c9e76c4bdda86e9bcc6663545c4afda6e4610b26b0f4e1347791f38733f855211dbc338d500399bc9c4360c5ba03dbd95eb029ebbee17dc3e7de8d24ede24c58dd100dc6dcb02835ca80b62dc1e72395c3f6f02fe57f43bda52191ba22c6671ed1f4ec1fb344d66e253b3f1e6a86b0a8891ef859316db955c16f6643dfdc8080e252322711d7da781cdbf374e7f243605fe4bc1c2b99cec53f08cf46e3382d09d20868b258a651bcaa3fac7e8204465ea6a440242a728431c57aa23c669b407a8ea0bf5e9184585053be59d970de18b4d406cc0d6f53c3d4273cbc85b198910269b61fcd0267b0c66273de4579aef4e90a2f9ac5e7ea405bb8d1f31c4de1703f5cced686e707656ce85e2268f2becdaa3a70c9728e3b37f8120fce0d96ae747c0ce5d9587546b391898320a46d07538786a72cded7d1aa78734b0eb348da9d7d1c3a245645af4ddace809a33c80ef777adbe074f0910962753c278f892a0edc0ab7702f32ff45790889d17e83030fb93fc362ce0fd7f0075faf4bc33e59ad18b27090469f10945d8c2d4126c4638faa8d7ff04152aa37956c75fefaf7ff2163fcc3423005f77fddfc1bd57520177f61670ab54d1396cf0163df6f35a56e9ddaa2d69b93985dd7afc542fbb195a87cdc210b4d8cda6803c7916c19a4673ec1e29f1ed6629e4bfb1a6df5199fde8b24099d05d49db8af47fb210d774ac10424387d5822596213fb73fde06a2f02ded69acbc1e436c1cb6881e88d5489ced8b5c689f7883c1b630c237d18e30391844a8a2b039a15cd547aab9986b07c90070bce10317c4743d7ef90f42a80d6404b72743be4bc5e1474cba4430e9f65762d2b87332b9ee93edf43c229cf5e6f8214e28fca64ac49719fe9c2a9bbb5ac1f875c3ebf907b31cd32695310f7cfa5c808e57d1b85f89213a190804c1c9ae59088fb1abc32513731b06ccf4e49661cf108e5e687241967c70a88eb9943fe53301ee0a4b5ec0e54e55b917d085327dbeda5fb082533ea20b6cd87fc504c08af66e8d7a2175263a67f99f54f76b25dd71f884450d17109e91b4bbeab0e9c8cbce0f16422e6bcb04b2c82fc0de3a30232d3573d148010f804e32da7d968a7d6ccadbd8919d2744fc5d5e07096388bccb66e4f820ce544efc27dbf801c28a05123cf8564473972482ddc00b31b68884eae6043aecd6e6bb988994546678aa33a0f8f538cf80459fbff9bb29aa4969c1f0709e306ae4a0fb8b1c7e0d0c678ecabb2ccbed3e91767002ec9d5b59f0f90a8ffd245536c116f04fa4ea8eff55d9f917ef1b91b9cdb5f584a82d84b08f9d6ceff783c5e6205236891b25b0b8063663bd23749e9a6081da26f28bf622a54781e25f60e49cd415d8a8d93d24dc08fa732ac912aae06a550b4f0ec96cf0c96e2edd68da8c5a21828d5dc97cf13b30fe",
      "rejectionSecret": "e8aee16342ebef3049d2d9497b61be6c52be8d756d4160b139abfdf716475cae"
    }
  ]
}
"""#
}
