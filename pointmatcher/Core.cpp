// kate: replace-tabs off; indent-width 4; indent-mode normal
// vim: ts=4:sw=4:noexpandtab
/*

Copyright (c) 2010--2011,
François Pomerleau and Stephane Magnenat, ASL, ETHZ, Switzerland
You can contact the authors at <f dot pomerleau at gmail dot com> and
<stephane at magnenat dot net>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETH-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "Core.h"
#include <cassert>
#include <iostream>
#include <boost/progress.hpp>

using namespace std;

template<typename T>
void MetricSpaceAligner<T>::DataPointsFilters::apply(DataPoints& cloud, bool iterate) const
{
	DataPoints filteredCloud;
	for (DataPointsFiltersConstIt it = this->begin(); it != this->end(); ++it)
	{
		filteredCloud = (*it)->filter(cloud, iterate);
		swapDataPoints<T>(cloud, filteredCloud);
	}
}

template<typename T>
void MetricSpaceAligner<T>::Transformations::apply(DataPoints& cloud, const TransformationParameters& parameters) const
{
	DataPoints transformedCloud;
	for (TransformationsConstIt it = this->begin(); it != this->end(); ++it)
	{
		transformedCloud = (*it)->compute(cloud, parameters);
		swapDataPoints<T>(cloud, transformedCloud);
	}
}

template<typename T>
void MetricSpaceAligner<T>::TransformationCheckers::init(const TransformationParameters& parameters, bool& iterate)
{
	for (TransformationCheckersIt it = this->begin(); it != this->end(); ++it)
		(*it)->init(parameters, iterate);
}

template<typename T>
void MetricSpaceAligner<T>::TransformationCheckers::check(const TransformationParameters& parameters, bool& iterate)
{
	for (TransformationCheckersIt it = this->begin(); it != this->end(); ++it)
		(*it)->check(parameters, iterate);
}

template<typename T>
typename MetricSpaceAligner<T>::OutlierWeights MetricSpaceAligner<T>::FeatureOutlierFilters::compute(const DataPoints& filteredReading, const DataPoints& filteredReference, const Matches& input, bool& iterate)
{
	//TODO: suboptimal, first multiplication by ones should be avoid
	OutlierWeights w = OutlierWeights::Ones(input.dists.rows(), input.dists.cols());
	for (FeatureOutlierFiltersIt it = this->begin(); it != this->end(); ++it)
		w.cwise() *= (*it)->compute(filteredReading, filteredReference, input, iterate);

	return w;
}

template<typename T>
MetricSpaceAligner<T>::ICP::ICP():
	matcher(0), 
	descriptorOutlierFilter(0),
	errorMinimizer(0),
	inspector(0),
	outlierMixingWeight(0.5)
{}

template<typename T>
MetricSpaceAligner<T>::ICP::~ICP()
{
	for (DataPointsFiltersIt it = readingDataPointsFilters.begin(); it != readingDataPointsFilters.end(); ++it)
		delete *it;
	for (DataPointsFiltersIt it = referenceDataPointsFilters.begin(); it != referenceDataPointsFilters.end(); ++it)
		delete *it;
	for (TransformationsIt it = transformations.begin(); it != transformations.end(); ++it)
		delete *it;
	delete matcher;
	for (FeatureOutlierFiltersIt it = featureOutlierFilters.begin(); it != featureOutlierFilters.end(); ++it)
		delete *it;
	delete descriptorOutlierFilter;
	delete errorMinimizer;
	for (TransformationCheckersIt it = transformationCheckers.begin(); it != transformationCheckers.end(); ++it)
		delete *it;
	delete inspector;
}

template<typename T>
typename MetricSpaceAligner<T>::TransformationParameters MetricSpaceAligner<T>::ICP::operator ()(
	const TransformationParameters& initialTransformationParameters, 
	DataPoints reading,
	DataPoints reference)
{
	boost::timer t; // Print how long take the algo

	assert(matcher);
	assert(descriptorOutlierFilter);
	assert(errorMinimizer);
	assert(inspector);
	
	// Move point clouds to their center of mass
	const int dim = reading.features.rows();
	const int nbPtsReading = reading.features.cols();
	const int nbPtsReference = reference.features.cols();

	Vector meanReading = reading.features.rowwise().sum();
	meanReading /= nbPtsReading;
	Vector meanReference = reference.features.rowwise().sum();
	meanReference /= nbPtsReference;

	for(int i=0; i < dim-1; i++)
	{
		//reading.features.row(i).cwise() -= meanReading(i);
		reference.features.row(i).cwise() -= meanReference(i);
	}
	
	Matrix Tread(Matrix::Identity(dim, dim));
	//Tread.block(0,dim-1, dim-1, 1) = meanReading.start(dim-1);
	
	Matrix Tref(Matrix::Identity(dim, dim));
	Tref.block(0,dim-1, dim-1, 1) = meanReference.start(dim-1);

	
	////


	bool iterate(true);
	
	readingDataPointsFilters.apply(reading, iterate);
	referenceDataPointsFilters.apply(reference, iterate);

	transformationCheckers.init(initialTransformationParameters, iterate);
	
	matcher->init(reference, iterate);

	inspector->init();
	
	TransformationParameters transformationParameters = Tref.inverse() * initialTransformationParameters;

	size_t iterationCount(0);
	
	cerr << "msa::icp - preprocess took " << t.elapsed() << " [s]" << endl;
	t.restart();
	
	while (iterate)
	{
		DataPoints stepReading(reading);
		DataPoints stepReference(reference);
		//strategy.readingDataPointsFilters.applyStep(stepReading, iterate);
		
		//strategy.referenceDataPointsFilters.applyStep(stepReference, iterate);
		
		//-----------------------------
		// Transform Readings
		transformations.apply(stepReading, transformationParameters);
		
		//-----------------------------
		// Match to closest point in Reference
		const Matches matches(
			matcher->findClosests(
				stepReading, 
				stepReference, 
				iterate)
		);
		
		//-----------------------------
		// Detect outliers
		const OutlierWeights featureOutlierWeights(
			featureOutlierFilters.compute(stepReading, stepReference, matches, iterate)
		);
		
		const OutlierWeights descriptorOutlierWeights(
			descriptorOutlierFilter->compute(
				stepReading, 
				stepReference, 
				matches, 
				iterate)
		);
		
		assert(featureOutlierWeights.rows() == matches.ids.rows());
		assert(featureOutlierWeights.cols() == matches.ids.cols());
		assert(descriptorOutlierWeights.rows() == matches.ids.rows());
		assert(descriptorOutlierWeights.cols() == matches.ids.cols());
		
		//cout << "featureOutlierWeights: " << featureOutlierWeights << "\n";
		//cout << "descriptorOutlierWeights: " << descriptorOutlierWeights << "\n";
		
		const OutlierWeights outlierWeights(
			featureOutlierWeights * outlierMixingWeight +
			descriptorOutlierWeights * (1 - outlierMixingWeight)
		);
		

		//-----------------------------
		// Write VTK files
		inspector->dumpIteration(iterationCount, transformationParameters, stepReference, stepReading, matches, featureOutlierWeights, descriptorOutlierWeights, transformationCheckers);
		
		//-----------------------------
		// Error minimization
		transformationParameters *= errorMinimizer->compute(stepReading, stepReference, outlierWeights, matches, iterate);
		
		transformationCheckers.check(Tref * transformationParameters, iterate);
		
		++iterationCount;
	}
	
	inspector->finish(iterationCount);
	
	cerr << "msa::icp - iterations took " << t.elapsed() << " [s]" << endl;
	
	// Move transformation back to original coordinate (without center of mass)
	return Tref * transformationParameters;
}

template<typename T>
MetricSpaceAligner<T>::ICPSequence::ICPSequence(const int dim):
	matcher(0), 
	descriptorOutlierFilter(0),
	errorMinimizer(0),
	inspector(0),
	outlierMixingWeight(0.5),
	ratioToSwitchKeyframe(0.8),
	keyFrameCreated(false),
	keyFrameTransform(Matrix::Identity(dim+1, dim+1)),
	keyFrameTransformOffset(Matrix::Identity(dim+1, dim+1)),
	curTransform(Matrix::Identity(dim+1, dim+1))
{}

template<typename T>
MetricSpaceAligner<T>::ICPSequence::~ICPSequence()
{
	for (DataPointsFiltersIt it = readingDataPointsFilters.begin(); it != readingDataPointsFilters.end(); ++it)
		delete *it;
	for (DataPointsFiltersIt it = keyframeDataPointsFilters.begin(); it != keyframeDataPointsFilters.end(); ++it)
		delete *it;
	for (TransformationsIt it = transformations.begin(); it != transformations.end(); ++it)
		delete *it;
	delete matcher;
	for (FeatureOutlierFiltersIt it = featureOutlierFilters.begin(); it != featureOutlierFilters.end(); ++it)
		delete *it;
	delete descriptorOutlierFilter;
	delete errorMinimizer;
	for (TransformationCheckersIt it = transformationCheckers.begin(); it != transformationCheckers.end(); ++it)
		delete *it;
	delete inspector;
}

template<typename T>
void MetricSpaceAligner<T>::ICPSequence::createKeyFrame(DataPoints& inputCloud)
{
	const int tDim(keyFrameTransform.rows());
	
	// apply filters
	bool iterate(true);
	keyframeDataPointsFilters.apply(inputCloud, iterate);
	if (!iterate)
		return;
	
	// center keyframe, retrieve offset
	const int nbPtsKeyframe = inputCloud.features.cols();
	const Vector meanKeyframe = inputCloud.features.rowwise().sum() / nbPtsKeyframe;
	for(int i=0; i < tDim-1; i++)
		inputCloud.features.row(i).cwise() -= meanKeyframe(i);
		
	// update keyframe
	keyFrameCloud = inputCloud;
	keyFrameTransformOffset.block(0,tDim-1, tDim-1, 1) = meanKeyframe.start(tDim-1);
	curTransform = Matrix::Identity(tDim, tDim);
	
	matcher->init(keyFrameCloud, iterate);
	
	keyFrameCreated = true;
}

template<typename T>
typename MetricSpaceAligner<T>::TransformationParameters MetricSpaceAligner<T>::ICPSequence::operator ()(
	DataPoints& inputCloud)
{
	boost::timer t; // Print how long take the algo

	assert(matcher);
	assert(descriptorOutlierFilter);
	assert(errorMinimizer);
	assert(inspector);
	
	// initial keyframe
	keyFrameCreated = false;
	if (keyFrameCloud.features.cols() == 0)
	{
		this->createKeyFrame(inputCloud);
		return curTransform;
	}
	
	////

	bool iterate(true);
	
	DataPoints reading(inputCloud);
	readingDataPointsFilters.apply(reading, iterate);
	
	transformationCheckers.init(curTransform, iterate);
	
	// FIXME: store kd-tree somewhere

	inspector->init();
	
	TransformationParameters transformationParameters = keyFrameTransformOffset.inverse() * curTransform;

	size_t iterationCount(0);
	
	cerr << "msa::icp - preprocess took " << t.elapsed() << " [s]" << endl;
	t.restart();
	
	while (iterate)
	{
		DataPoints stepReading(reading);
		
		//-----------------------------
		// Transform Readings
		transformations.apply(stepReading, transformationParameters);
		
		//-----------------------------
		// Match to closest point in Reference
		const Matches matches(
			matcher->findClosests(
				stepReading, 
				keyFrameCloud, 
				iterate)
		);
		
		//-----------------------------
		// Detect outliers
		const OutlierWeights featureOutlierWeights(
			featureOutlierFilters.compute(
				stepReading, 
				keyFrameCloud, 
				matches, 
				iterate)
		);
		
		const OutlierWeights descriptorOutlierWeights(
			descriptorOutlierFilter->compute(
				stepReading, 
				keyFrameCloud, 
				matches, 
				iterate)
		);
		
		assert(featureOutlierWeights.rows() == matches.ids.rows());
		assert(featureOutlierWeights.cols() == matches.ids.cols());
		assert(descriptorOutlierWeights.rows() == matches.ids.rows());
		assert(descriptorOutlierWeights.cols() == matches.ids.cols());
		
		//cout << "featureOutlierWeights: " << featureOutlierWeights << "\n";
		//cout << "descriptorOutlierWeights: " << descriptorOutlierWeights << "\n";
		
		const OutlierWeights outlierWeights(
			featureOutlierWeights * outlierMixingWeight +
			descriptorOutlierWeights * (1 - outlierMixingWeight)
		);

		//-----------------------------
		// Dump
		inspector->dumpIteration(iterationCount, transformationParameters, keyFrameCloud, stepReading, matches, featureOutlierWeights, descriptorOutlierWeights, transformationCheckers);
		
		//-----------------------------
		// Error minimization
		transformationParameters *= errorMinimizer->compute(stepReading, keyFrameCloud, outlierWeights, matches, iterate);
		
		transformationCheckers.check(keyFrameTransformOffset * transformationParameters, iterate);
		
		++iterationCount;
	}
	
	inspector->finish(iterationCount);
	
	// Move transformation back to original coordinate (without center of mass)
	curTransform = keyFrameTransformOffset * transformationParameters;
	
	if (errorMinimizer->getWeightedPointUsedRatio() < ratioToSwitchKeyframe)
	{
		// new keyframe
		keyFrameTransform *= curTransform;
		this->createKeyFrame(inputCloud);
	}
	
	cerr << "msa::icp - iterations took " << t.elapsed() << " [s]" << endl;
	
	// Return transform in world space
	return keyFrameTransform * curTransform;
}

template struct MetricSpaceAligner<float>;
template struct MetricSpaceAligner<double>;

