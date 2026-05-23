import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';
import { Counter } from 'k6/metrics';
import { textSummary } from 'https://jslib.k6.io/k6-summary/0.0.1/index.js';
import exec from 'k6/execution';

const testData = new SharedArray('test-data', function () {
    return JSON.parse(open('./test-data.json')).entries;
});
const statsArr = new SharedArray('test-stats', function () {
    return [JSON.parse(open('./test-data.json')).stats];
});
const expectedStats = statsArr[0];

const tpCount = new Counter('tp_count');
const tnCount = new Counter('tn_count');
const fpCount = new Counter('fp_count');
const fnCount = new Counter('fn_count');
const errorCount = new Counter('error_count');

const fpEntries = [];

export const options = {
    // Adicionado 'avg' e 'med' para você ter mais visibilidade do tempo durante o teste
    summaryTrendStats: ['avg', 'med', 'p(99)'],
    systemTags: ['status', 'method'],
    dns: {
        ttl: '5m',
        select: 'roundRobin',
    },
    scenarios: {
        default: {
            executor: 'ramping-arrival-rate',
            startRate: 1,
            timeUnit: '1s',
            preAllocatedVUs: 100,
            maxVUs: 250,
            gracefulStop: '10s',
            stages: [
                { duration: '120s', target: 900 },
            ],
        },
    },
};

export function setup() {
    console.log(
        `\n=== INICIANDO TESTE ===`
        + `\nDataset: ${expectedStats.total} entries, `
        + `${expectedStats.fraud_count} fraud (${expectedStats.fraud_rate}%), `
        + `${expectedStats.legit_count} legit (${expectedStats.legit_rate}%), `
        + `edge cases: ${expectedStats.edge_case_rate}%\n`
    );
}

export default function () {
    const idx = exec.scenario.iterationInTest;
    if (idx >= testData.length) return;
    const entry = testData[idx];
    const expectedApproved = entry.expected_approved;

    const res = http.post(
        'http://localhost:9999/fraud-score',
        JSON.stringify(entry.request),
        { headers: { 'Content-Type': 'application/json' }, timeout: '2001ms' }
    );

    if (res.status === 200) {
        const body = JSON.parse(res.body);
        if (expectedApproved === body.approved) {
            if (body.approved) tnCount.add(1); 
            else tpCount.add(1);               
        } else {
            if (body.approved) fnCount.add(1);
            else {
                fpCount.add(1);
                console.warn(`[FALSO POSITIVO] idx=${idx} - Transação legítima bloqueada! Request: ${JSON.stringify(entry.request)}`);
                fpEntries.push({ idx, request: entry.request });
            }
        }
    } else {
        errorCount.add(1);
    }

    // PRINT EM TEMPO REAL: A cada 5000 requisições globais do teste,
    // ele joga um snapshot rápido de como está a acurácia no terminal
    if (idx % 5000 === 0 && idx > 0) {
        console.log(`[Progresso] Requisições processadas: ${idx}...`);
    }
}

export function handleSummary(data) {
    const K = 1000;
    const T_MAX_MS = 1000;
    const P99_MIN_MS = 1;
    const P99_MAX_MS = 2000;
    const EPSILON_MIN = 0.001;
    const BETA = 300;
    const TX_CORTE = 0.15;
    const SCORE_P99_CORTE = -3000;
    const SCORE_DET_CORTE = -3000;

    const httpDuration = data.metrics.http_req_duration.values;
    const p99 = httpDuration['p(99)'];

    const tp = data.metrics.tp_count ? data.metrics.tp_count.values.count : 0;
    const tn = data.metrics.tn_count ? data.metrics.tn_count.values.count : 0;
    const fp = data.metrics.fp_count ? data.metrics.fp_count.values.count : 0;
    const fn = data.metrics.fn_count ? data.metrics.fn_count.values.count : 0;
    const errs = data.metrics.error_count ? data.metrics.error_count.values.count : 0;

    const N = tp + tn + fp + fn + errs;

    const E = (fp * 1) + (fn * 3) + (errs * 5);
    const failures = fp + fn + errs;
    const epsilon = N > 0 ? E / N : 0;
    const failureRate = N > 0 ? failures / N : 0;

    let p99Score;
    let p99CutTriggered = false;
    if (p99 <= 0) {
        p99Score = 0;
    } else if (p99 > P99_MAX_MS) {
        p99Score = SCORE_P99_CORTE;
        p99CutTriggered = true;
    } else {
        p99Score = K * Math.log10(T_MAX_MS / Math.max(p99, P99_MIN_MS));
    }

    let detScore;
    let rateComponent = 0;
    let absolutePenalty = 0;
    let cutTriggered = false;
    if (failureRate > TX_CORTE) {
        detScore = SCORE_DET_CORTE;
        cutTriggered = true;
    } else {
        rateComponent = K * Math.log10(1 / Math.max(epsilon, EPSILON_MIN));
        absolutePenalty = -BETA * Math.log10(1 + E);
        detScore = rateComponent + absolutePenalty;
    }

    const finalScore = p99Score + detScore;

    const result = {
        expected: expectedStats,
        p99: p99.toFixed(2) + 'ms',
        scoring: {
            breakdown: {
                false_positive_detections: fp,
                false_negative_detections: fn,
                true_positive_detections: tp,
                true_negative_detections: tn,
                http_errors: errs,
            },
            failure_rate: +(failureRate * 100).toFixed(2) + '%',
            weighted_errors_E: E,
            error_rate_epsilon: +epsilon.toFixed(6),
            p99_score: {
                value: +p99Score.toFixed(2),
                cut_triggered: p99CutTriggered,
            },
            detection_score: {
                value: +detScore.toFixed(2),
                rate_component: cutTriggered ? null : +rateComponent.toFixed(2),
                absolute_penalty: cutTriggered ? null : +absolutePenalty.toFixed(2),
                cut_triggered: cutTriggered,
            },
            final_score: +finalScore.toFixed(2),
        },
    };

    // LOG DO SCORE FINAL NO TERMINAL: Mostra um resumão direto na tela antes de fechar
    console.log(`\n=== RESULTADOS DO SCORE ===`);
    console.log(`Final Score:    ${finalScore.toFixed(2)}`);
    console.log(`P99 Latency:    ${p99.toFixed(2)}ms (Score: ${p99Score.toFixed(2)})`);
    console.log(`Failure Rate:   ${(failureRate * 100).toFixed(2)}% (Fails: ${failures} de ${N})`);
    console.log(`Métricas:       TP: ${tp} | TN: ${tn} | FP: ${fp} | FN: ${fn} | Erros: ${errs}`);
    console.log(`===========================\n`);

    if (fpEntries.length > 0) {
        console.log(`\n=== FALSOS POSITIVOS (${fpEntries.length} transações legítimas bloqueadas) ===`);
        for (const e of fpEntries) {
            console.log(`  [idx=${e.idx}] ${JSON.stringify(e.request)}`);
        }
        console.log(`=======================================================\n`);
    }

    return {
        'test/results.json': JSON.stringify(result, null, 2),
        // MUDANÇA: Descomentado para ativar o output padrão detalhado do k6 por cima do JSON
        stdout: textSummary(data, { indent: ' ', enableColors: true }),
    };
}