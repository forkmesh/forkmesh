package com.forkmesh.nativeapp.feature.codeinsight

import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.InsightTrendPoint

class CodeInsightContext(
    val snapshot: InsightSnapshot,
    val repoTitle: String,
    val branch: String,
    val sourceLabel: String,
    val openFile: (path: String, line: Int) -> Unit,
    val loadTrend: suspend (path: String) -> List<InsightTrendPoint>,
    val reanalyze: () -> Unit,
    val setSummary: (String) -> Unit,
)
