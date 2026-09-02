package modelregistry

import (
	"errors"
	"math/big"
)

type metricAccumulator struct {
	count      uint64
	sum        big.Int
	sumSquares big.Int
}

func (accumulator *metricAccumulator) add(value int64) {
	term := big.NewInt(value)
	accumulator.sum.Add(&accumulator.sum, term)
	var square big.Int
	square.Mul(term, term)
	accumulator.sumSquares.Add(&accumulator.sumSquares, &square)
	accumulator.count++
}

func (accumulator *metricAccumulator) interval(metric RollbackMetric, threshold RollbackThreshold, confidenceZPPM uint32) (ConfidenceInterval, error) {
	result := ConfidenceInterval{
		Metric: metric, SampleCount: accumulator.count, Threshold: threshold.MaximumRegression,
		EnoughSamples: accumulator.count >= threshold.MinimumSamples,
	}
	if accumulator.count == 0 {
		return result, nil
	}
	n := new(big.Int).SetUint64(accumulator.count)
	meanFloor := floorDiv(&accumulator.sum, n)
	meanCeil := ceilDiv(&accumulator.sum, n)
	mean := new(big.Int).Quo(new(big.Int).Set(&accumulator.sum), n)
	meanValue, ok := mean.Int64(), mean.IsInt64()
	if !ok {
		return ConfidenceInterval{}, errors.New("mean delta exceeds int64")
	}
	result.MeanDelta = meanValue
	if accumulator.count < 2 {
		floorValue, floorOK := meanFloor.Int64(), meanFloor.IsInt64()
		ceilValue, ceilOK := meanCeil.Int64(), meanCeil.IsInt64()
		if !floorOK || !ceilOK {
			return ConfidenceInterval{}, errors.New("confidence bound exceeds int64")
		}
		result.LowerBound = floorValue
		result.UpperBound = ceilValue
		return result, nil
	}

	// Paired sample mean standard error:
	// sqrt((n*sum(x^2)-sum(x)^2) / (n*n*(n-1))). All arithmetic is exact
	// big-integer arithmetic; the square root and scaling round upward.
	var numerator big.Int
	numerator.Mul(n, &accumulator.sumSquares)
	var sumSquared big.Int
	sumSquared.Mul(&accumulator.sum, &accumulator.sum)
	numerator.Sub(&numerator, &sumSquared)
	if numerator.Sign() < 0 {
		return ConfidenceInterval{}, errors.New("negative variance numerator")
	}
	var denominator big.Int
	denominator.Mul(n, n)
	denominator.Mul(&denominator, new(big.Int).Sub(n, big.NewInt(1)))
	varianceOfMeanCeil := ceilDiv(&numerator, &denominator)
	standardError := ceilSqrt(varianceOfMeanCeil)
	var scaled big.Int
	scaled.Mul(standardError, new(big.Int).SetUint64(uint64(confidenceZPPM)))
	halfWidthBig := ceilDiv(&scaled, big.NewInt(partsPerMillion))
	if !halfWidthBig.IsUint64() {
		return ConfidenceInterval{}, errors.New("confidence half-width exceeds uint64")
	}
	result.HalfWidth = halfWidthBig.Uint64()
	var lower big.Int
	lower.Sub(meanFloor, halfWidthBig)
	var upper big.Int
	upper.Add(meanCeil, halfWidthBig)
	if !lower.IsInt64() || !upper.IsInt64() {
		return ConfidenceInterval{}, errors.New("confidence interval exceeds int64")
	}
	result.LowerBound = lower.Int64()
	result.UpperBound = upper.Int64()
	result.Triggered = result.EnoughSamples && result.UpperBound > threshold.MaximumRegression
	return result, nil
}

func ceilDiv(numerator *big.Int, denominator *big.Int) *big.Int {
	quotient := new(big.Int)
	remainder := new(big.Int)
	quotient.QuoRem(numerator, denominator, remainder)
	if remainder.Sign() != 0 && numerator.Sign() > 0 {
		quotient.Add(quotient, big.NewInt(1))
	}
	return quotient
}

func floorDiv(numerator *big.Int, denominator *big.Int) *big.Int {
	quotient := new(big.Int)
	remainder := new(big.Int)
	quotient.QuoRem(numerator, denominator, remainder)
	if remainder.Sign() != 0 && numerator.Sign() < 0 {
		quotient.Sub(quotient, big.NewInt(1))
	}
	return quotient
}

func ceilSqrt(value *big.Int) *big.Int {
	root := new(big.Int).Sqrt(value)
	var square big.Int
	square.Mul(root, root)
	if square.Cmp(value) < 0 {
		root.Add(root, big.NewInt(1))
	}
	return root
}
