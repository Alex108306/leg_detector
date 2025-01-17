#include "leg_detector/cluster_features.h"
#include <opencv2/core/core.hpp>

std::vector<float> ClusterFeatures::calcClusterFeatures(const laser_processor::SampleSet *cluster, const sensor_msgs::msg::LaserScan &scan)
{
    // Number of points
    int num_points = cluster->size();

    // Compute mean and median points for future use
    float x_mean = 0.0;
    float y_mean = 0.0;
    std::vector<float> x_median_set;
    std::vector<float> y_median_set;
    for (laser_processor::SampleSet::iterator i = cluster->begin(); i != cluster->end(); i++)
    {
        x_mean += ((*i)->x) / num_points;
        y_mean += ((*i)->y) / num_points;
        x_median_set.push_back((*i)->x);
        y_median_set.push_back((*i)->y);
    }

    std::sort(x_median_set.begin(), x_median_set.end());
    std::sort(y_median_set.begin(), y_median_set.end());

    float x_median = 0.5 * (*(x_median_set.begin() + (num_points - 1) / 2) + *(x_median_set.begin() + num_points / 2));
    float y_median = 0.5 * (*(y_median_set.begin() + (num_points - 1) / 2) + *(y_median_set.begin() + num_points / 2));

    // Computer distance to laser scanner
    float distance = sqrt(x_median * x_median + y_median * y_median);

    // Compute std and avg diff from median
    double sum_std_diff = 0.0;
    double sum_med_diff = 0.0;

    for (laser_processor::SampleSet::iterator i = cluster->begin(); i != cluster->end(); i++)
    {
        sum_std_diff += pow((*i)->x - x_mean, 2) + pow((*i)->y - y_mean, 2);
        sum_med_diff += sqrt(pow((*i)->x - x_median, 2) + pow((*i)->y - y_median, 2));
    }

    float std = sqrt(1.0 / (num_points - 1.0) * sum_std_diff);
    float avg_median_dev = sum_med_diff / num_points;

    // Get first and last points in cluster
    laser_processor::SampleSet::iterator first = cluster->begin();
    laser_processor::SampleSet::iterator last = cluster->end();
    --last;

    // Compute Jump distance and Occluded right and Occluded left
    int prev_ind = (*first)->index - 1;
    int next_ind = (*last)->index + 1;

    float occluded_left = 1;
    float occluded_right = 1;

    if (prev_ind >= 0)
    {
        laser_processor::Sample *prev = laser_processor::Sample::Extract(prev_ind, scan);
        if (prev != NULL)
        {
            if ((*first)->range < prev->range or prev->range < 0.01)
                occluded_left = 0;

            delete prev;
        }
    }

    if (next_ind < static_cast<int>(scan.ranges.size()))
    {
        laser_processor::Sample *next = laser_processor::Sample::Extract(next_ind, scan);
        if (next != NULL)
        {
            if ((*last)->range < next->range or next->range < 0.01)
                occluded_right = 0;

            delete next;
        }
    }

    // Compute width - euclidian distance between first + last points
    float width = sqrt(pow((*first)->x - (*last)->x, 2) + pow((*first)->y - (*last)->y, 2));

    // Compute Linearity
    cv::Mat *points = new cv::Mat(num_points, 2, CV_64FC1);
    {
        int j = 0;
        for (laser_processor::SampleSet::iterator i = cluster->begin(); i != cluster->end(); i++)
        {
            points->at<double>(j, 0) = (*i)->x - x_mean;
            points->at<double>(j, 1) = (*i)->y - y_mean;
            j++;
        }
    }

    cv::Mat *W = new cv::Mat(2, 2, CV_64FC1);
    cv::Mat *U = new cv::Mat(num_points, 2, CV_64FC1);
    cv::Mat *V = new cv::Mat(2, 2, CV_64FC1);
    cv::SVD::compute(*points, *W, *U, *V);

    cv::Mat *rot_points = new cv::Mat(num_points, 2, CV_64FC1);
    cv::gemm(*U, *W, 1.0, cv::Mat(), 0.0, *rot_points);

    float linearity = 0.0;
    for (int i = 0; i < num_points; i++)
    {
        linearity += std::pow(rot_points->at<double>(i, 1), 2);
    }

    delete points;
    points = nullptr;

    delete W;
    W = nullptr;

    delete U;
    U = nullptr;

    delete V;
    V = nullptr;

    delete rot_points;
    rot_points = nullptr;

    // Compute Circularity
    cv::Mat *A = new cv::Mat(num_points, 3, CV_64FC1);
    cv::Mat *B = new cv::Mat(num_points, 1, CV_64FC1);

    {
        int j = 0;
        for (laser_processor::SampleSet::iterator i = cluster->begin(); i != cluster->end(); i++)
        {
            float x = (*i)->x;
            float y = (*i)->y;

            A->at<double>(j, 0) = -2.0 * x;
            A->at<double>(j, 1) = -2.0 * y;

            A->at<double>(j, 2) = 1;

            B->at<double>(j, 0) = -pow(x, 2) - pow(y, 2);
            j++;
        }
    }

    cv::Mat *sol = new cv::Mat(3, 1, CV_64FC1);

    cv::solve(*A, *B, *sol, cv::DecompTypes::DECOMP_SVD);

    double xc = sol->at<double>(0, 0);
    double yc = sol->at<double>(1, 0);
    double rc = sqrt(pow(xc, 2) + pow(yc, 2) - sol->at<double>(2, 0));

    delete A;
    A = nullptr;

    delete B;
    B = nullptr;

    delete sol;
    sol = nullptr;

    float circularity = 0.0;
    for (laser_processor::SampleSet::iterator i = cluster->begin(); i != cluster->end(); i++)
    {
        circularity += pow(rc - sqrt(pow(xc - (*i)->x, 2) + pow(yc - (*i)->y, 2)), 2);
    }

    // Radius
    float radius = rc;

    // Curvature:
    float mean_curvature = 0.0;

    // Boundary length:
    float boundary_length = 0.0;
    float last_boundary_seg = 0.0;

    float boundary_regularity = 0.0;
    double sum_boundary_reg_sq = 0.0;

    // Mean angular difference
    laser_processor::SampleSet::iterator left = cluster->begin();
    left++;
    left++;
    laser_processor::SampleSet::iterator mid = cluster->begin();
    mid++;
    laser_processor::SampleSet::iterator right = cluster->begin();

    float ang_diff = 0.0;

    while (left != cluster->end())
    {
        float mlx = (*left)->x - (*mid)->x;
        float mly = (*left)->y - (*mid)->y;
        float L_ml = sqrt(mlx * mlx + mly * mly);

        float mrx = (*right)->x - (*mid)->x;
        float mry = (*right)->y - (*mid)->y;
        float L_mr = sqrt(mrx * mrx + mry * mry);

        float lrx = (*left)->x - (*right)->x;
        float lry = (*left)->y - (*right)->y;
        float L_lr = sqrt(lrx * lrx + lry * lry);

        boundary_length += L_mr;
        sum_boundary_reg_sq += L_mr * L_mr;
        last_boundary_seg = L_ml;

        float A = (mlx * mrx + mly * mry) / pow(L_mr, 2);
        float B = (mlx * mry - mly * mrx) / pow(L_mr, 2);

        float th = atan2(B, A);

        if (th < 0)
            th += 2 * M_PI;

        ang_diff += th / num_points;

        float s = 0.5 * (L_ml + L_mr + L_lr);
        float area = sqrt(s * (s - L_ml) * (s - L_mr) * (s - L_lr));

        if (th > 0)
            mean_curvature += 4 * (area) / (L_ml * L_mr * L_lr * num_points);
        else
            mean_curvature -= 4 * (area) / (L_ml * L_mr * L_lr * num_points);

        left++;
        mid++;
        right++;
    }

    boundary_length += last_boundary_seg;
    sum_boundary_reg_sq += last_boundary_seg * last_boundary_seg;

    boundary_regularity = sqrt((sum_boundary_reg_sq - pow(boundary_length, 2) / num_points) / (num_points - 1));

    // Mean angular difference
    first = cluster->begin();
    mid = cluster->begin();
    mid++;
    last = cluster->end();
    last--;

    double sum_iav = 0.0;
    double sum_iav_sq = 0.0;

    while (mid != last)
    {
        float mlx = (*first)->x - (*mid)->x;
        float mly = (*first)->y - (*mid)->y;

        float mrx = (*last)->x - (*mid)->x;
        float mry = (*last)->y - (*mid)->y;
        float L_mr = sqrt(mrx * mrx + mry * mry);

        float A = (mlx * mrx + mly * mry) / pow(L_mr, 2);
        float B = (mlx * mry - mly * mrx) / pow(L_mr, 2);

        float th = atan2(B, A);

        if (th < 0)
            th += 2 * M_PI;

        sum_iav += th;
        sum_iav_sq += th * th;

        mid++;
    }

    // incribed angle variance?
    float iav = sum_iav / num_points;
    float std_iav = sqrt((sum_iav_sq - pow(sum_iav, 2) / num_points) / (num_points - 1));

    // Add features
    std::vector<float> features;

    // features from "Using Boosted Features for the Detection of People in 2D Range Data"
    features.push_back(num_points);
    features.push_back(std);
    features.push_back(avg_median_dev);
    features.push_back(width);
    features.push_back(linearity);
    features.push_back(circularity);
    features.push_back(radius);
    features.push_back(boundary_length);
    features.push_back(boundary_regularity);
    features.push_back(mean_curvature);
    features.push_back(ang_diff);
    // feature from paper which cannot be calculated here: mean speed

    // Inscribed angular variance, I believe. Not sure what paper this is from
    features.push_back(iav);
    features.push_back(std_iav);

    // New features from Angus
    features.push_back(distance);
    features.push_back(distance / num_points);
    features.push_back(occluded_right);
    features.push_back(occluded_left);

    return features;
}
